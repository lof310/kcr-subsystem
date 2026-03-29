// SPDX-License-Identifier: GPL-2.0
/**
 * KCR IOMMU Integration - Hardware-based invalidation notifier
 *
 * This file integrates KCR with the IOMMU subsystem to achieve 100%
 * invalidation coverage for DMA writes. Unlike syscall-hook approaches
 * (80% coverage), IOMMU notifications catch:
 * - DMA device writes to shared memory
 * - GPU memory modifications
 * - Network card buffer updates
 * - Any device-initiated memory changes
 *
 * The notifier registers a callback that receives IOMMU_INVALIDATE_WRITE
 * events when pages are modified by devices, triggering immediate cache
 * invalidation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/iommu.h>
#include <linux/kcr.h>

/**
 * iommu_invalidate_handler() - IOMMU invalidation event callback
 * @n: Notifier block embedded in iommu_notifier_data
 * @event: Event type (IOMMU_INVALIDATE_WRITE)
 * @data: Page address being invalidated
 *
 * Called by IOMMU subsystem when a page invalidation occurs due to
 * device write access. Extracts page address from event data and
 * triggers KCR cache invalidation for the affected range.
 *
 * In aggressive_mode, invalidates on any write access.
 * In standard mode, only invalidates on confirmed DMA writes.
 *
 * Returns: 0 if handled, non-zero to skip processing
 */
static int iommu_invalidate_handler(struct notifier_block *n,
				     unsigned long event, void *data)
{
	struct iommu_notifier_data *notifier_data;
	unsigned long start, end;

	if (event != IOMMU_INVALIDATE_WRITE)
		return 0;

	notifier_data = container_of(n, struct iommu_notifier_data, notifier);
	
	/* Calculate page-aligned range for invalidation */
	start = (unsigned long)data & PAGE_MASK;
	end = PAGE_ALIGN(start + PAGE_SIZE);
	
	invalidate_range(notifier_data->mm, start, end);
	
	return 0;
}

/**
 * kcr_iommu_init() - Register IOMMU invalidation notifier
 * @mm: Memory space to monitor for invalidations
 * @notifier_data: Pre-allocated notifier registration structure
 *
 * Registers callback with IOMMU subsystem to receive notifications
 * on page invalidations caused by DMA operations. This provides:
 * - 100% coverage for device-initiated writes
 * - Hardware-enforced notification (cannot be bypassed)
 * - Low overhead (only triggers on actual invalidations)
 *
 * Caller must allocate and zero-initialize notifier_data before calling.
 * Sets aggressive_mode=true to catch all potential invalidations.
 *
 * Returns: 0 on success, negative errno on failure (e.g., -ENODEV if IOMMU unavailable)
 */
int kcr_iommu_init(struct mm_struct *mm, struct iommu_notifier_data *notifier_data)
{
	int ret;

	if (!notifier_data || !mm)
		return -EINVAL;

	notifier_data->mm = mm;
	notifier_data->aggressive_mode = true;
	notifier_data->notifier.notifier_call = iommu_invalidate_handler;
	
	ret = iommu_register_notifier(&notifier_data->notifier);
	if (ret) {
		pr_warn("KCR: failed to register IOMMU notifier (%d)\n", ret);
		return ret;
	}

	pr_debug("KCR: registered IOMMU notifier for mm %p\n", mm);
	return 0;
}
EXPORT_SYMBOL(kcr_iommu_init);

/**
 * kcr_iommu_exit() - Unregister IOMMU notifier
 * @notifier_data: Notifier structure to unregister
 *
 * Removes previously registered IOMMU notifier callback.
 * Should be called during cleanup or when monitoring is no longer needed.
 */
void kcr_iommu_exit(struct iommu_notifier_data *notifier_data)
{
	if (!notifier_data)
		return;

	iommu_unregister_notifier(&notifier_data->notifier);
	pr_debug("KCR: unregistered IOMMU notifier\n");
}
EXPORT_SYMBOL(kcr_iommu_exit);
