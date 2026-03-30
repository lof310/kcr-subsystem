// SPDX-License-Identifier: GPL-2.0
/**
 * KCR Debugfs Interface - Statistics and configuration via debugfs
 *
 * This file implements the debugfs interface for KCR monitoring and debugging:
 * - /sys/kernel/debug/kcr/stats: Real-time cache statistics
 * - /sys/kernel/debug/kcr/config: Current configuration parameters
 *
 * Statistics include:
 * - Global counters (L2 hits, L3 hits, misses, invalidations)
 * - Hit rate percentage
 * - Per-CPU breakdown for NUMA analysis
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/kcr.h>

/* External symbols from kcr_main.c */
extern struct cpu_cache __percpu *caches;
extern struct l3_table *l3_tables;
extern int num_sockets;
extern bool kcr_enable;
extern struct shared_region *kcr_region;

static struct dentry *kcr_debugfs_root;

/**
 * show_stats() - Display cache statistics in seq_file format
 * @m: Seq_file buffer for output
 * @v: Private data (unused)
 *
 * Aggregates per-CPU statistics and displays:
 * - Global enable status and region allocation
 * - Total L2/L3 hits, misses, invalidations
 * - Calculated hit rate percentage
 * - Per-CPU breakdown for performance analysis
 *
 * Returns: 0 on success
 */
static int show_stats(struct seq_file *m, void *v)
{
	struct cpu_cache *cache;
	int cpu;
	u64 total_l2_hits = 0, total_l3_hits = 0;
	u64 total_misses = 0, total_invalidations = 0;
	u64 total_cycles_saved = 0;

	seq_printf(m, "KCR Statistics\n");
	seq_printf(m, "==============\n\n");
	seq_printf(m, "Enabled: %s\n", kcr_enable ? "yes" : "no");
	seq_printf(m, "Shared Region: %s\n", kcr_region ? "allocated" : "not allocated");
	seq_printf(m, "Sockets: %d\n\n", num_sockets);

	/* Aggregate per-CPU statistics */
	for_each_possible_cpu(cpu) {
		cache = per_cpu_ptr(caches, cpu);
		total_l2_hits += cache->stats.l2_hits;
		total_l3_hits += cache->stats.l3_hits;
		total_misses += cache->stats.misses;
		total_invalidations += cache->stats.invalidations;
		total_cycles_saved += cache->stats.cycles_saved;
	}

	seq_printf(m, "Global Statistics:\n");
	seq_printf(m, "  L2 Hits:          %llu\n", total_l2_hits);
	seq_printf(m, "  L3 Hits:          %llu\n", total_l3_hits);
	seq_printf(m, "  Misses:           %llu\n", total_misses);
	seq_printf(m, "  Invalidations:    %llu\n", total_invalidations);
	seq_printf(m, "  Cycles Saved:     %llu\n", total_cycles_saved);
	
	/* Calculate and display hit rate */
	if (total_l2_hits + total_l3_hits + total_misses > 0) {
		u64 total = total_l2_hits + total_l3_hits + total_misses;
		u32 hit_rate = (total_l2_hits + total_l3_hits) * 100 / total;
		seq_printf(m, "  Hit Rate:         %u%%\n", hit_rate);
	}

	seq_printf(m, "\nPer-CPU Statistics:\n");
	for_each_possible_cpu(cpu) {
		cache = per_cpu_ptr(caches, cpu);
		seq_printf(m, "  CPU %d: L2=%llu L3=%llu Miss=%llu Inv=%llu\n",
			   cpu, cache->stats.l2_hits, cache->stats.l3_hits,
			   cache->stats.misses, cache->stats.invalidations);
	}

	return 0;
}

/**
 * stats_open() - Open handler for stats file
 * @inode: Inode of opened file
 * @file: File structure
 *
 * Wrapper for single_open() to set up seq_file for stats display.
 *
 * Returns: Result from single_open()
 */
static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, show_stats, inode->i_private);
}

static const struct file_operations stats_fops = {
	.owner = THIS_MODULE,
	.open = stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * show_config() - Display configuration parameters in seq_file format
 * @m: Seq_file buffer for output
 * @v: Private data (unused)
 *
 * Displays compile-time and runtime configuration:
 * - CONFIG_KCR status
 * - Region size, L2/L3 entry counts
 * - Maximum result size
 *
 * Returns: 0 on success
 */
static int show_config(struct seq_file *m, void *v)
{
	seq_printf(m, "KCR Configuration\n");
	seq_printf(m, "=================\n\n");
	seq_printf(m, "CONFIG_KCR: y\n");
	seq_printf(m, "Region Size: %u MB\n", KCR_REGION_SIZE / (1024 * 1024));
	seq_printf(m, "L2 Entries: %d (per-CPU)\n", KCR_L2_ENTRIES);
	seq_printf(m, "L3 Entries: %d (per-socket)\n", KCR_L3_ENTRIES);
	seq_printf(m, "Max Result Size: %d bytes\n", KCR_RESULT_SIZE_MAX);
	
	return 0;
}

/**
 * config_open() - Open handler for config file
 * @inode: Inode of opened file
 * @file: File structure
 *
 * Wrapper for single_open() to set up seq_file for config display.
 *
 * Returns: Result from single_open()
 */
static int config_open(struct inode *inode, struct file *file)
{
	return single_open(file, show_config, inode->i_private);
}

static const struct file_operations config_fops = {
	.owner = THIS_MODULE,
	.open = config_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * kcr_debugfs_init() - Initialize debugfs interface
 *
 * Creates /sys/kernel/debug/kcr directory with two read-only files:
 * - stats: Real-time cache statistics
 * - config: Configuration parameters
 *
 * Called during KCR initialization. Failure is non-fatal (warning only).
 *
 * Returns: 0 on success, negative errno on failure
 */
int kcr_debugfs_init(void)
{
	kcr_debugfs_root = debugfs_create_dir("kcr", NULL);
	if (IS_ERR(kcr_debugfs_root))
		return PTR_ERR(kcr_debugfs_root);

	debugfs_create_file("stats", 0444, kcr_debugfs_root, NULL, &stats_fops);
	debugfs_create_file("config", 0444, kcr_debugfs_root, NULL, &config_fops);

	pr_info("KCR: debugfs initialized at /sys/kernel/debug/kcr\n");
	return 0;
}
EXPORT_SYMBOL(kcr_debugfs_init);

/**
 * kcr_debugfs_exit() - Remove debugfs interface
 *
 * Recursively removes kcr debugfs directory and all child entries.
 * Called during KCR cleanup.
 */
void kcr_debugfs_exit(void)
{
	debugfs_remove_recursive(kcr_debugfs_root);
	kcr_debugfs_root = NULL;
}
EXPORT_SYMBOL(kcr_debugfs_exit);
