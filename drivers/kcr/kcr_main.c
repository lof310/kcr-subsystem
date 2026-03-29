// SPDX-License-Identifier: GPL-2.0
/**
 * KCR Main Module - Core initialization and lifecycle management
 *
 * This file implements the main entry points for the Kernel Computation Reuse (KCR)
 * subsystem. It handles:
 * - Module initialization and cleanup
 * - Per-CPU L2 cache allocation and initialization
 * - Per-socket L3 cache allocation and initialization
 * - Debugfs interface setup
 * - Module parameter handling
 *
 * Architecture:
 *   L2 Cache: 512 entries per-CPU, RCU-protected hash table
 *   L3 Cache: 4096 entries per-socket, shared among CPUs on same NUMA node
 *   Shared Region: 16 MB memfd-backed anonymous memory
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kcr.h>

MODULE_AUTHOR("KCR Project");
MODULE_DESCRIPTION("Kernel Computation Reuse - Adaptive Transparent Lookup Acceleration");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

/* Module parameter to enable/disable KCR at runtime */
static bool kcr_enable = true;
module_param(kcr_enable, bool, 0644);
MODULE_PARM_DESC(kcr_enable, "Enable KCR subsystem (default: on)");

/* Global shared memory region - visible to kernel and user space */
static struct shared_region *kcr_region;

/**
 * caches - Per-CPU L2 cache instances
 *
 * Each CPU core maintains its own independent L2 cache to eliminate
 * inter-core contention. Aligned to cache line boundaries (64 bytes)
 * to prevent false sharing between adjacent CPUs.
 *
 * Exported for use by cache operations in kcr_cache.c
 */
DEFINE_PER_CPU_ALIGNED(struct cpu_cache, caches);
EXPORT_PER_CPU_SYMBOL(caches);

/* Per-socket L3 cache tables - one table per NUMA node/socket */
static struct l3_table *l3_tables;
static int num_sockets;

/* Export global symbols for use by other KCR modules */
EXPORT_SYMBOL(l3_tables);
EXPORT_SYMBOL(num_sockets);
EXPORT_SYMBOL(kcr_enable);
EXPORT_SYMBOL(kcr_region);

/**
 * kcr_is_enabled() - Check if KCR subsystem is active
 *
 * Returns true only if both the module parameter enables KCR
 * and the shared memory region has been successfully allocated.
 *
 * Returns: true if KCR is operational, false otherwise
 */
bool kcr_is_enabled(void)
{
	return kcr_enable && kcr_region != NULL;
}
EXPORT_SYMBOL(kcr_is_enabled);

/**
 * kcr_init() - Initialize KCR subsystem
 *
 * Performs complete initialization of the KCR subsystem:
 * 1. Allocates 16 MB shared memory region via memfd
 * 2. Initializes per-CPU L2 caches (hash buckets + spinlocks)
 * 3. Allocates and initializes per-socket L3 caches
 * 4. Sets up debugfs interface for statistics and configuration
 *
 * Called automatically during module load or kernel boot.
 *
 * Returns: 0 on success, negative errno on failure (-ENOMEM if allocation fails)
 */
int kcr_init(void)
{
	int cpu, ret;

	if (!kcr_enable) {
		pr_info("KCR: disabled by module parameter\n");
		return 0;
	}

	/* Allocate shared memory region (memfd-backed, zero-copy) */
	kcr_region = kcr_alloc_region();
	if (!kcr_region) {
		pr_err("KCR: failed to allocate shared region\n");
		return -ENOMEM;
	}

	/* Determine number of NUMA nodes (sockets) for L3 cache allocation */
	num_sockets = num_possible_nodes();
	l3_tables = kcalloc(num_sockets, sizeof(struct l3_table), GFP_KERNEL);
	if (!l3_tables) {
		ret = -ENOMEM;
		goto err_free_region;
	}

	/* Initialize per-CPU L2 caches */
	for_each_possible_cpu(cpu) {
		int i;
		struct cpu_cache *cache = per_cpu_ptr(&caches, cpu);
		
		for (i = 0; i < KCR_L2_ENTRIES; i++) {
			INIT_HLIST_HEAD(&cache->l2[i].head);
			spin_lock_init(&cache->l2[i].lock);
		}
	}

	/* Initialize per-socket L3 caches */
	for (cpu = 0; cpu < num_sockets; cpu++) {
		int i;
		
		l3_tables[cpu].socket_id = cpu;
		for (i = 0; i < KCR_L3_ENTRIES; i++) {
			INIT_HLIST_HEAD(&l3_tables[cpu].buckets[i].head);
			spin_lock_init(&l3_tables[cpu].buckets[i].lock);
		}
	}

	/* Create debugfs interface for monitoring and debugging */
	ret = kcr_debugfs_init();
	if (ret) {
		pr_warn("KCR: debugfs initialization failed (%d)\n", ret);
	}

	pr_info("KCR: initialized with %d sockets, %d CPUs\n", num_sockets, num_possible_cpus());
	return 0;

err_free_region:
	kcr_free_region(kcr_region);
	kcr_region = NULL;
	return ret;
}
EXPORT_SYMBOL(kcr_init);

/**
 * kcr_exit() - Cleanup KCR subsystem
 *
 * Performs complete teardown of the KCR subsystem:
 * 1. Removes debugfs interface
 * 2. Frees per-socket L3 cache tables
 * 3. Releases shared memory region (reference counted)
 *
 * Called automatically during module unload or kernel shutdown.
 * Per-CPU caches are automatically freed by the per-CPU subsystem.
 */
void kcr_exit(void)
{
	kcr_debugfs_exit();
	
	if (l3_tables) {
		kfree(l3_tables);
		l3_tables = NULL;
	}
	
	if (kcr_region) {
		kcr_free_region(kcr_region);
		kcr_region = NULL;
	}
	
	pr_info("KCR: unloaded\n");
}
EXPORT_SYMBOL(kcr_exit);

module_init(kcr_init);
module_exit(kcr_exit);
