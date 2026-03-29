/* SPDX-License-Identifier: GPL-2.0 */
/**
 * KCR VMA Flag Definitions for mmap Integration
 *
 * This header defines the VM_KCR flag used to mark virtual memory areas
 * as eligible for KCR caching. When set, the kernel tracks execution
 * history and caches deterministic computation results for that VMA.
 *
 * Integration requires adding this flag definition to mm_types.h:
 *   #define VM_KCR		0x01000000UL
 *
 * Usage from user space (via mmap flags):
 *   void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
 *                     MAP_PRIVATE | MAP_ANONYMOUS | VM_KCR, -1, 0);
 */

#ifndef _LINUX_KCR_FLAGS_H
#define _LINUX_KCR_FLAGS_H

#ifdef CONFIG_KCR

/**
 * VM_KCR - VMA flag for KCR caching eligibility
 *
 * Marks a VMA as eligible for Kernel Computation Reuse caching.
 * When this flag is set:
 * - The kernel tracks execution history for determinism verification
 * - After 100 consistent executions, results are cached transparently
 * - Cache hits bypass computation, returning stored results directly
 *
 * Uses bit 24 (0x01000000) of vm_flags. This bit position should be
 * verified against other VM_* flags in mm_types.h to avoid conflicts.
 *
 * Integration note: Add to mm_types.h alongside other VM_* flags:
 *   #define VM_KCR		0x01000000UL
 */
#define VM_KCR		0x01000000UL

#else /* !CONFIG_KCR */

/* Stub definition for !CONFIG_KCR builds - evaluates to 0 */
#define VM_KCR		0

#endif /* CONFIG_KCR */

#endif /* _LINUX_KCR_FLAGS_H */
