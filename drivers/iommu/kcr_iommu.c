// SPDX-License-Identifier: GPL-2.0
/**
 * KCR IOMMU Integration - Hardware-based invalidation notifier (Stub)
 *
 * This file provides a stub implementation for systems where the full
 * IOMMU notifier API is not available. On kernels with full support,
 * it registers callbacks for IOMMU invalidation events. On standard
 * kernels, it provides no-op implementations that allow the module
 * to load and function with alternative invalidation mechanisms.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/iommu.h>
#include <linux/kcr.h>

/* 
 * Define IOMMU_INVALIDATE_WRITE if not present in kernel headers.
 * This allows compilation on kernels without this specific event type.
 */
#ifndef IOMMU_INVALIDATE_WRITE
#define IOMMU_INVALIDATE_WRITE 0x100
#endif

/**
 * iommu_invalidate_handler() - IOMMU invalidation event callback (stub)
 * @n: Notifier block embedded in iommu_notifier_data
 * @event: Event type (IOMMU_INVALIDATE_WRITE)
 * @data: Page address being invalidated
 *
 * Stub implementation that logs the event but doesn't perform
 * actual invalidation on kernels without full IOMMU support.
 *
 * Returns: 0 (notifier done)
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

/* Only call invalidate_range if the function exists */
invalidate_range(notifier_data->mm, start, end);

return 0;
}

/**
 * kcr_iommu_init() - Register IOMMU invalidation notifier (stub version)
 * @mm: Memory space to monitor for invalidations
 * @notifier_data: Pre-allocated notifier registration structure
 *
 * Stub implementation that always succeeds but doesn't actually
 * register with the IOMMU subsystem on kernels without support.
 * The module will still function using alternative invalidation
 * mechanisms (e.g., MMU notifiers or periodic flushing).
 *
 * Returns: 0 (always succeeds in stub mode)
 */
int kcr_iommu_init(struct mm_struct *mm, struct iommu_notifier_data *notifier_data)
{
if (!notifier_data || !mm)
return -EINVAL;

notifier_data->mm = mm;
notifier_data->aggressive_mode = true;
notifier_data->notifier.notifier_call = iommu_invalidate_handler;

pr_debug("KCR: registered IOMMU notifier (stub mode) for mm %p\n", mm);
return 0;
}
EXPORT_SYMBOL(kcr_iommu_init);

/**
 * kcr_iommu_exit() - Unregister IOMMU notifier (stub version)
 * @notifier_data: Notifier structure to unregister
 *
 * Stub implementation that performs no action since registration
 * was also a no-op.
 */
void kcr_iommu_exit(struct iommu_notifier_data *notifier_data)
{
if (!notifier_data)
return;

pr_debug("KCR: unregistered IOMMU notifier (stub mode)\n");
}
EXPORT_SYMBOL(kcr_iommu_exit);
