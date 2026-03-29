/* SPDX-License-Identifier: GPL-2.0 */
/**
 * KCR Integration Points for mm_types.h
 *
 * This header defines extension fields that must be added to core kernel
 * structures when CONFIG_KCR is enabled. These extensions enable:
 * - VMA metadata tracking for determinism verification
 * - MM generation counters for cache invalidation detection
 *
 * Integration requires patching mm_types.h to add these fields:
 *   In struct vm_area_struct:
 *     #ifdef CONFIG_KCR
 *     struct vma_metadata *kcr_metadata;
 *     #endif
 *
 *   In struct mm_struct:
 *     #ifdef CONFIG_KCR
 *     u64 kcr_generation;
 *     #endif
 */

#ifndef _LINUX_KCR_TYPES_H
#define _LINUX_KCR_TYPES_H

#include <linux/atomic.h>

#ifdef CONFIG_KCR

/* Forward declarations */
struct vma_metadata;

/**
 * struct vm_area_struct extension - kcr_metadata field
 *
 * Pointer to VMA-specific metadata for determinism tracking.
 * Allocated on first access, inherited across fork() with COW semantics.
 * Contains execution history, verification state, and result samples.
 *
 * Integration note: Add to struct vm_area_struct in mm_types.h:
 *   #ifdef CONFIG_KCR
 *   struct vma_metadata *kcr_metadata;
 *   #endif
 */

/**
 * struct mm_struct extension - kcr_generation field
 *
 * Generation counter incremented on every mm modification (mmap, munmap, etc.).
 * Used by cache entries to detect stale references after mm_struct reuse.
 * Prevents cross-process cache pollution from recycled mm_struct addresses.
 *
 * Integration note: Add to struct mm_struct in mm_types.h:
 *   #ifdef CONFIG_KCR
 *   u64 kcr_generation;
 *   #endif
 */

#else /* !CONFIG_KCR */

/* Stub declaration for !CONFIG_KCR builds */
struct vma_metadata;

#endif /* CONFIG_KCR */

#endif /* _LINUX_KCR_TYPES_H */
