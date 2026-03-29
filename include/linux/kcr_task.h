/* SPDX-License-Identifier: GPL-2.0 */
/**
 * KCR Task Structure Extensions for sched.h Integration
 *
 * This header defines per-task data structures that must be embedded in
 * task_struct when CONFIG_KCR is enabled. These extensions provide:
 * - Per-process encryption keys for result protection
 * - Process-specific fingerprint seeds for isolation
 * - Per-task KCR enable/disable control
 *
 * Integration requires patching sched.h to add this field to task_struct:
 *   #ifdef CONFIG_KCR
 *   struct kcr_task_struct *kcr_task;
 *   #endif
 */

#ifndef _LINUX_KCR_TASK_H
#define _LINUX_KCR_TASK_H

#include <linux/types.h>

#ifdef CONFIG_KCR

/**
 * struct kcr_task_struct - Per-task KCR metadata
 * @kcr_process_key: 256-bit AES key for encrypting cached results (optional)
 * @kcr_enabled: Per-task KCR enable flag (can override global setting)
 * @kcr_seed: Process-unique seed for fingerprint computation
 *
 * Embedded in task_struct when CONFIG_KCR is enabled.
 * Provides process-level isolation and security features:
 * - Unique fingerprint seeds prevent cross-process cache pollution
 * - Optional encryption protects sensitive computation results
 * - Per-task enable allows fine-grained control
 *
 * Integration note: Add to struct task_struct in sched.h:
 *   #ifdef CONFIG_KCR
 *   struct kcr_task_struct *kcr_task;
 *   #endif
 */
struct kcr_task_struct {
	/* Per-process encryption key (optional, used if CONFIG_KCR_ENCRYPTION) */
	u8 kcr_process_key[32];
	/* KCR enabled flag for this task */
	bool kcr_enabled;
	/* Process-specific fingerprint seed for isolation */
	u64 kcr_seed;
};

#else /* !CONFIG_KCR */

/* Forward declaration for !CONFIG_KCR builds */
struct kcr_task_struct;

#endif /* CONFIG_KCR */

#endif /* _LINUX_KCR_TASK_H */
