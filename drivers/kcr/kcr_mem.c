// SPDX-License-Identifier: GPL-2.0
/**
 * KCR Memory Management - Simplified vmalloc-based allocation
 *
 * This file implements shared memory management for KCR using vmalloc.
 * The shared region is visible to kernel space. User space access
 * requires additional integration via mmap or other mechanisms.
 *
 * Key features:
 * - Simple vmalloc allocation (no memfd dependency)
 * - Reference counting for safe concurrent access
 * - Compatible with standard kernels without patches
 *
 * Memory layout:
 *   Offset 0:         Metadata (4 KB)
 *   Offset 4 KB:      L2 cache entries (32 KB)
 *   Offset 36 KB:     L3 cache entries (256 KB)
 *   Remaining:        Free space for future expansion
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/kcr.h>

/* MFD flags - defined for compatibility but not used in simplified version */
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
 * kcr_alloc_region() - Allocate shared memory region via vmalloc
 *
 * Creates a kernel virtual memory region using vmalloc().
 * This simplified implementation doesn't require memfd support
 * and works on standard kernels.
 *
 * Returns: Pointer to shared_region structure, or NULL on failure
 */
struct shared_region *kcr_alloc_region(void)
{
	struct shared_region *region;
	void *vaddr;

	/* Allocate region descriptor */
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return NULL;

	/* Allocate with vmalloc - simple and reliable */
	vaddr = vmalloc(KCR_REGION_SIZE);
	if (!vaddr) {
		pr_warn("KCR: vmalloc failed\n");
		kfree(region);
		return NULL;
	}

	/* Initialize region structure */
	region->memfd_file = NULL;  /* No memfd in simplified version */
	region->kernel_vaddr = vaddr;
	region->size = KCR_REGION_SIZE;
	region->phys_addr = 0;  /* vmalloc doesn't provide contiguous physical */
	atomic_set(&region->refcount, 1);

	/* Zero-initialize the region */
	memset(vaddr, 0, KCR_REGION_SIZE);

	pr_info("KCR: allocated %lu MB shared region via vmalloc\n",
		(unsigned long)(KCR_REGION_SIZE / (1024 * 1024)));

	return region;
}
EXPORT_SYMBOL(kcr_alloc_region);

/**
 * kcr_free_region() - Free shared memory region
 *
 * Unmaps kernel virtual address and frees descriptor.
 * Uses reference counting for safe concurrent access.
 *
 * @region: Region to free
 */
void kcr_free_region(struct shared_region *region)
{
	if (!region)
		return;

	if (!atomic_dec_and_test(&region->refcount))
		return;

	if (region->kernel_vaddr) {
		vfree(region->kernel_vaddr);
		region->kernel_vaddr = NULL;
	}

	/* No memfd file to release in simplified version */

	kfree(region);
	pr_debug("KCR: freed shared region\n");
}
EXPORT_SYMBOL(kcr_free_region);

/**
 * kcr_map_to_user() - Map shared region to user space (stub)
 *
 * In the full implementation, this would use remap_pfn_range()
 * or vm_insert_page() to map the region into user space.
 * This simplified version returns -ENOSYS as user mapping
 * requires additional kernel support.
 *
 * @region: Shared region to map
 * @task: Target task for mapping
 * Returns: -ENOSYS (not implemented in simplified version)
 */
int kcr_map_to_user(struct shared_region *region, struct task_struct *task)
{
	if (!region || !task)
		return -EINVAL;

	/* 
	 * Simplified version doesn't support user mapping.
	 * Full implementation would use:
	 * - remap_pfn_range() for physical mappings
	 * - vm_insert_page() for vmalloc regions
	 * - Or memfd_get_fd() + sys_mmap() for memfd-backed regions
	 */
	pr_debug("KCR: user mapping not available in simplified mode\n");
	return -ENOSYS;
}
EXPORT_SYMBOL(kcr_map_to_user);
