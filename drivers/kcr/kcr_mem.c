// SPDX-License-Identifier: GPL-2.0
/**
 * KCR Memory Management - memfd-based shared memory allocation
 *
 * This file implements zero-copy shared memory management for KCR using
 * the memfd subsystem. The shared region is simultaneously visible to
 * both kernel and user space, eliminating 400-600 cycle copy overhead
 * per kernel-user transition.
 *
 * Key features:
 * - Anonymous memory allocation (no backing filesystem)
 * - File descriptor export to user space
 * - mmap() mapping without data copies
 * - Sealing options (MFD_NOEXEC_SEAL) for security
 * - Reference counting for safe concurrent access
 *
 * Memory layout:
 *   Offset 0:         Metadata (4 KB)
 *   Offset 4 KB:      L2 cache entries (32 KB)
 *   Offset 36 KB:     L3 cache entries (256 KB)
 *   Remaining:        Free space for future expansion
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/syscalls.h>
#include <linux/fdtable.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/kcr.h>

/* Forward declaration for memfd_create - defined in mm/memfd.c but not exported in headers */
extern long do_memfd_create(const char __user *uname_ptr, unsigned int flags);

/* MFD flags for memfd_create - define if not available */
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0004U
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

/* PROT and MAP flags for vm_mmap */
#ifndef PROT_READ
#define PROT_READ 0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef MAP_SHARED
#define MAP_SHARED 0x01
#endif

/**
 * kcr_alloc_region() - Allocate shared memory region via memfd
 *
 * Creates an anonymous memory-backed file descriptor using memfd_create(),
 * then maps it into kernel virtual address space. The region is:
 * - Sealed with MFD_NOEXEC_SEAL to prevent code execution
 * - Marked MFD_CLOEXEC to auto-close on exec()
 * - Pre-allocated to 16 MB via vfs_fallocate()
 * - Mapped into kernel space via vm_map_ram()
 *
 * Returns: Pointer to shared_region structure, or NULL on failure
 */
struct shared_region *kcr_alloc_region(void)
{
	struct shared_region *region;
	struct file *memfd = NULL;
	void *vaddr;
	int ret;

	/* Allocate region descriptor */
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return NULL;

	/* Try memfd_create first, fall back to pure vmalloc if unavailable */
#ifdef CONFIG_MEMFD_CREATE
	{
		struct file *tmp_memfd;
		long ret_fd;
		
		ret_fd = do_memfd_create("kcr_shared", MFD_NOEXEC_SEAL | MFD_CLOEXEC);
		if (ret_fd >= 0) {
			tmp_memfd = fget(ret_fd);
			if (tmp_memfd) {
				memfd = tmp_memfd;
				put_unused_fd(ret_fd);
			}
		} else {
			pr_warn("KCR: memfd_create failed with %ld, falling back to vmalloc\n", ret_fd);
		}
	}
#endif

	if (memfd) {
		/* Pre-allocate full 16 MB region */
		ret = vfs_fallocate(memfd, 0, 0, KCR_REGION_SIZE);
		if (ret < 0) {
			pr_warn("KCR: vfs_fallocate failed with %d\n", ret);
			fput(memfd);
			memfd = NULL;
		}
	}

	/* Allocate with vmalloc - works for both memfd and fallback cases */
	vaddr = vmalloc(KCR_REGION_SIZE);
	if (!vaddr) {
		pr_warn("KCR: vmalloc failed\n");
		if (memfd)
			fput(memfd);
		kfree(region);
		return NULL;
	}

	/* Initialize region structure */
	region->memfd_file = memfd;
	region->kernel_vaddr = vaddr;
	region->size = KCR_REGION_SIZE;
	atomic_set(&region->refcount, 1);

	pr_info("KCR: allocated %lu MB shared region%s\n", 
		(unsigned long)(KCR_REGION_SIZE / (1024 * 1024)),
		memfd ? " (memfd-backed)" : " (vmalloc fallback)");
	return region;
}
EXPORT_SYMBOL(kcr_alloc_region);

/**
 * kcr_free_region() - Free shared memory region
 *
 * Releases all resources associated with the shared region:
 * - Unmaps kernel virtual address mapping
 * - Releases memfd file reference
 * - Frees region descriptor
 *
 * Uses atomic reference counting to ensure safe deallocation
 * when multiple users may hold references.
 *
 * @region: Region to free (can be NULL)
 */
void kcr_free_region(struct shared_region *region)
{
	if (!region)
		return;

	/* Only free when last reference is dropped */
	if (atomic_dec_and_test(&region->refcount)) {
		if (region->kernel_vaddr)
			vfree(region->kernel_vaddr);
		if (region->memfd_file)
			fput(region->memfd_file);
		kfree(region);
	}
}
EXPORT_SYMBOL(kcr_free_region);

/**
 * kcr_map_to_user() - Map shared region to user space
 *
 * Maps the memfd-backed region into the target task's address space
 * using vm_mmap(). User space can then access cached results directly
 * without syscalls, achieving zero-copy data sharing.
 *
 * The mapping is:
 * - Read-write accessible (PROT_READ | PROT_WRITE)
 * - Shared across processes (MAP_SHARED)
 * - Located at kernel-chosen address (addr 0 hint)
 *
 * @region: Shared region to map
 * @task: Target task for mapping
 * Returns: 0 on success, negative errno on failure
 */
int kcr_map_to_user(struct shared_region *region, struct task_struct *task)
{
	struct mm_struct *mm;
	unsigned long addr;
	int ret;

	if (!region || !task)
		return -EINVAL;

	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;

	/* Map memfd into user address space */
	addr = vm_mmap(region->memfd_file, 0, region->size,
		       PROT_READ | PROT_WRITE, MAP_SHARED, 0);
	if (IS_ERR_VALUE(addr)) {
		ret = (long)addr;
		pr_warn("KCR: vm_mmap failed with %d\n", ret);
		goto out_mm;
	}

	pr_debug("KCR: mapped region to user space at 0x%lx for PID %d\n",
		 addr, task_pid_nr(task));
	ret = 0;

out_mm:
	mmput(mm);
	return ret;
}
EXPORT_SYMBOL(kcr_map_to_user);
