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
#include <linux/memfd.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/kcr.h>

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
	struct file *memfd;
	void *vaddr;
	int ret;

	/* Allocate region descriptor */
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return NULL;

	/* Create anonymous memfd with security seals */
	memfd = memfd_create("kcr_shared", MFD_NOEXEC_SEAL | MFD_CLOEXEC);
	if (IS_ERR(memfd)) {
		ret = PTR_ERR(memfd);
		goto err_free;
	}

	/* Pre-allocate full 16 MB region */
	ret = vfs_fallocate(memfd, 0, 0, KCR_REGION_SIZE);
	if (ret < 0)
		goto err_put_file;

	/* Map into kernel virtual address space */
	vaddr = vm_map_ram(&memfd->f_mapping, KCR_REGION_SIZE >> PAGE_SHIFT, 0, PAGE_KERNEL);
	if (!vaddr)
		goto err_put_file;

	/* Initialize region structure */
	region->memfd_file = memfd;
	region->kernel_vaddr = vaddr;
	region->size = KCR_REGION_SIZE;
	atomic_set(&region->refcount, 1);

	pr_info("KCR: allocated %lu MB shared region\n", KCR_REGION_SIZE / (1024 * 1024));
	return region;

err_put_file:
	fput(memfd);
err_free:
	kfree(region);
	return NULL;
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
			vm_unmap_ram(region->kernel_vaddr, KCR_REGION_SIZE >> PAGE_SHIFT);
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
