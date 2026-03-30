// SPDX-License-Identifier: GPL-2.0
/**
 * KCR Determinism Verification - Learning mode for VMA validation
 *
 * This file implements the determinism verification system that ensures
 * only deterministic code paths are cached. It uses a learning mode approach:
 *
 * States:
 * - UNVERIFIED: Initial state, collecting execution samples (100 iterations)
 * - VERIFIED: Confirmed deterministic after 100 consistent results
 * - REJECTED: Non-deterministic detected, permanently excluded from caching
 *
 * This prevents caching of functions with:
 * - Side effects (I/O operations, random number generation)
 * - Timing dependencies (rdtsc, performance counters)
 * - External inputs (network data, user input)
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/kcr.h>

/**
 * verify_deterministic() - Verify function determinism via learning mode
 * @meta: VMA metadata structure tracking execution history
 * @current_result: Most recent execution result (fingerprint)
 *
 * Implements three-state machine for determinism detection:
 *
 * UNVERIFIED state:
 * - On first execution (count=0): Store result as baseline
 * - On subsequent executions: Compare against baseline
 *   - Match: Increment count, store in ring buffer
 *   - Mismatch: Transition to REJECTED immediately
 * - After 100 consistent executions: Transition to VERIFIED
 *
 * VERIFIED state:
 * - Always return true (caching allowed)
 *
 * REJECTED state:
 * - Always return false (caching permanently disabled)
 *
 * Returns: true if execution appears deterministic, false otherwise
 */
bool verify_deterministic(struct vma_metadata *meta, u64 current_result)
{
	if (!meta)
		return false;

	switch (meta->state) {
	case KCR_VMA_UNVERIFIED:
		/* First execution: establish baseline */
		if (meta->execution_count == 0) {
			meta->first_result = current_result;
		} else if (meta->first_result != current_result) {
			/* Mismatch detected: reject this VMA */
			meta->state = KCR_VMA_REJECTED;
			return false;
		}
		
		/* Store result in ring buffer for analysis */
		meta->result_history[meta->execution_count % 4] = current_result;
		meta->execution_count++;
		
		/* After 100 consistent results, mark as verified */
		if (meta->execution_count >= 100)
			meta->state = KCR_VMA_VERIFIED;
		
		return true;
		
	case KCR_VMA_VERIFIED:
		/* Already verified: allow caching */
		return true;
		
	case KCR_VMA_REJECTED:
		/* Previously rejected: never cache */
		return false;
	}
	
	return false;
}
EXPORT_SYMBOL(verify_deterministic);

/**
 * should_cache() - Determine if VMA should be cached
 * @vma: Virtual memory area to evaluate
 *
 * Checks two conditions for caching eligibility:
 * 1. VM_KCR flag must be set (user opted-in via mmap flags)
 * 2. VMA metadata must exist and be in VERIFIED state
 *
 * UNVERIFIED VMAs enter learning mode but don't get cached yet.
 * REJECTED VMAs are permanently excluded from caching.
 *
 * Returns: true if caching is allowed, false otherwise
 */
bool should_cache(struct vm_area_struct *vma)
{
	struct vma_metadata *meta;

	/* VM_KCR flag doesn't exist in standard kernel, so we skip this check */
	/* In a patched kernel, you would check: if (!vma || !(vma->vm_flags & VM_KCR)) */
	if (!vma)
		return false;

	/* kcr_metadata doesn't exist in standard kernel, return false to disable caching */
	/* In a patched kernel, you would access: meta = vma->kcr_metadata; */
	return false;  /* Disabled for unpatched kernels */
}
EXPORT_SYMBOL(should_cache);
