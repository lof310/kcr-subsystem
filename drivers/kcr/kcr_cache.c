// SPDX-License-Identifier: GPL-2.0
/**
 * KCR Cache Implementation - Two-tier cache hierarchy
 *
 * This file implements the two-tier cache hierarchy for KCR:
 * - L2 Cache: Per-CPU, 512 entries, RCU-protected hash table
 *   Hit latency: 15-25 cycles, zero inter-core contention
 * - L3 Cache: Per-socket, 4096 entries, shared among CPUs
 *   Hit latency: 50-100 cycles, larger capacity
 *
 * Design principles:
 * - Cache-line alignment (64 bytes) prevents false sharing
 * - RCU enables lock-free reads for high-performance lookups
 * - Spinlocks protect writes with IRQ safety
 * - L3 hits promote entries to L2 for faster subsequent access
 * - TTL-based expiration (1 second default) ensures freshness
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/percpu-defs.h>
#include <linux/kcr.h>

/* External symbols from kcr_main.c */
extern struct cpu_cache __percpu *caches;
extern struct l3_table *l3_tables;
extern int num_sockets;

/**
 * free_entry_rcu() - RCU callback for deferred entry freeing
 * @head: RCU head embedded in kcr_entry.node
 *
 * Called by RCU subsystem after grace period completes.
 * Safely frees cache entry when all readers have finished.
 */
static void free_entry_rcu(struct rcu_head *head)
{
	struct kcr_entry *entry;
	
	entry = container_of(head, struct kcr_entry, node);
	kfree(entry);
}

/**
 * lookup_l2() - Search per-CPU L2 cache
 * @cache: Per-CPU cache structure
 * @fingerprint: 64-bit hash to match
 * @mm: Memory space for isolation check
 *
 * Performs lock-free RCU read-side traversal of L2 hash bucket.
 * Checks fingerprint match, mm isolation, and TTL expiration.
 *
 * Returns: Matching entry pointer, or NULL on miss
 */
static struct kcr_entry *lookup_l2(struct cpu_cache *cache, u64 fingerprint,
				    struct mm_struct *mm)
{
	u32 bucket = fingerprint % KCR_L2_ENTRIES;
	struct l2_bucket *bkt = &cache->l2[bucket];
	struct kcr_entry *entry;

	rcu_read_lock();
	hlist_for_each_entry_rcu(entry, &bkt->head, node) {
		if (entry->fingerprint == fingerprint && entry->mm == mm) {
			if (time_before(jiffies, entry->expiry_jiffies)) {
				rcu_read_unlock();
				this_cpu_inc(cache->stats.l2_hits);
				return entry;
			}
		}
	}
	rcu_read_unlock();
	return NULL;
}

/**
 * lookup_l3() - Search per-socket L3 cache
 * @fingerprint: 64-bit hash to match
 * @mm: Memory space for isolation check
 *
 * Traverses L3 hash table for current socket. Uses same matching
 * criteria as L2 (fingerprint, mm, TTL). No statistics update
 * here - caller tracks L3 hits separately.
 *
 * Returns: Matching entry pointer, or NULL on miss
 */
static struct kcr_entry *lookup_l3(u64 fingerprint, struct mm_struct *mm)
{
	int socket = cpu_to_node(smp_processor_id());
	struct l3_table *table;
	struct kcr_entry *entry;

	if (socket >= num_sockets)
		socket = 0;
	
	table = &l3_tables[socket];
	rcu_read_lock();
	hlist_for_each_entry_rcu(entry, &table->buckets[fingerprint % KCR_L3_ENTRIES].head, node) {
		if (entry->fingerprint == fingerprint && entry->mm == mm) {
			if (time_before(jiffies, entry->expiry_jiffies)) {
				rcu_read_unlock();
				return entry;
			}
		}
	}
	rcu_read_unlock();
	return NULL;
}

/**
 * promote_to_l2() - Move entry from L3 to L2 cache
 * @cache: Target per-CPU cache
 * @entry: Entry to promote
 * @fingerprint: Hash for bucket calculation
 *
 * On L3 hit, promotes entry to L2 for faster subsequent access.
 * Removes from L3 list, inserts at head of L2 bucket (MRU position).
 * Uses spinlock with IRQ save for concurrent access safety.
 */
static void promote_to_l2(struct cpu_cache *cache, struct kcr_entry *entry,
			  u64 fingerprint)
{
	u32 bucket = fingerprint % KCR_L2_ENTRIES;
	struct l2_bucket *bkt = &cache->l2[bucket];
	unsigned long flags;

	spin_lock_irqsave(&bkt->lock, flags);
	hlist_del_rcu(&entry->node);
	hlist_add_head_rcu(&entry->node, &bkt->head);
	spin_unlock_irqrestore(&bkt->lock, flags);
}

/**
 * lookup_unified() - Unified two-tier cache lookup
 * @fingerprint: 64-bit hash to search for
 * @mm: Memory space for isolation
 *
 * Performs hierarchical cache lookup:
 * 1. Check L2 (fast path, 15-25 cycles)
 * 2. On miss, check L3 (slower, 50-100 cycles)
 * 3. On L3 hit, promote to L2 for locality
 * 4. Update statistics counters
 *
 * Returns: Matching entry pointer, or NULL if not found
 */
struct kcr_entry *lookup_unified(u64 fingerprint, struct mm_struct *mm)
{
	struct cpu_cache *cache;
	struct kcr_entry *entry;

	cache = this_cpu_ptr(&caches);
	
	entry = lookup_l2(cache, fingerprint, mm);
	if (entry)
		return entry;
	
	entry = lookup_l3(fingerprint, mm);
	if (entry) {
		promote_to_l2(cache, entry, fingerprint);
		this_cpu_inc(cache->stats.l3_hits);
		return entry;
	}
	
	this_cpu_inc(cache->stats.misses);
	return NULL;
}
EXPORT_SYMBOL(lookup_unified);

/**
 * store_in_l2() - Insert entry into per-CPU L2 cache
 * @cache: Target per-CPU cache
 * @entry: Entry to store
 * @fingerprint: Hash for bucket selection
 *
 * Replaces any existing entry with same fingerprint+mm.
 * Uses RCU-safe deletion with deferred freeing via call_rcu().
 * Inserts new entry at head of bucket (MRU position).
 *
 * Returns: 0 on success
 */
static int store_in_l2(struct cpu_cache *cache, struct kcr_entry *entry,
		       u64 fingerprint)
{
	u32 bucket = fingerprint % KCR_L2_ENTRIES;
	struct l2_bucket *bkt = &cache->l2[bucket];
	struct kcr_entry *old;
	unsigned long flags;

	spin_lock_irqsave(&bkt->lock, flags);
	
	/* Remove existing entry with same key */
	hlist_for_each_entry_rcu(old, &bkt->head, node) {
		if (old->fingerprint == fingerprint && old->mm == entry->mm) {
			hlist_del_rcu(&old->node);
			call_rcu(&old->node, free_entry_rcu);
			break;
		}
	}
	
	hlist_add_head_rcu(&entry->node, &bkt->head);
	spin_unlock_irqrestore(&bkt->lock, flags);
	return 0;
}

/**
 * store_in_l3() - Insert entry into per-socket L3 cache
 * @entry: Entry to store
 * @fingerprint: Hash for bucket selection
 *
 * Similar to store_in_l2() but operates on L3 table.
 * Determines socket via cpu_to_node(), handles edge case
 * where socket ID exceeds allocated tables.
 *
 * Returns: 0 on success
 */
static int store_in_l3(struct kcr_entry *entry, u64 fingerprint)
{
	int socket = cpu_to_node(smp_processor_id());
	struct l3_table *table;
	struct kcr_entry *old;
	unsigned long flags;

	if (socket >= num_sockets)
		socket = 0;
	
	table = &l3_tables[socket];
	spin_lock_irqsave(&table->buckets[fingerprint % KCR_L3_ENTRIES].lock, flags);
	
	/* Remove existing entry with same key */
	hlist_for_each_entry_rcu(old, &table->buckets[fingerprint % KCR_L3_ENTRIES].head, node) {
		if (old->fingerprint == fingerprint && old->mm == entry->mm) {
			hlist_del_rcu(&old->node);
			call_rcu(&old->node, free_entry_rcu);
			break;
		}
	}
	
	hlist_add_head_rcu(&entry->node, &table->buckets[fingerprint % KCR_L3_ENTRIES].head);
	spin_unlock_irqrestore(&table->buckets[fingerprint % KCR_L3_ENTRIES].lock, flags);
	return 0;
}

/**
 * store_result() - Store computation result in both cache tiers
 * @fingerprint: 64-bit hash of input data
 * @data: Result data to cache (max 64 bytes)
 * @len: Length of result data
 * @mm: Owner memory space for isolation
 *
 * Allocates and initializes new cache entry:
 * - Copies result data (up to 64 bytes)
 * - Sets TTL expiration (1 second from now)
 * - Records current mm_generation for validation
 * - Captures PID and namespace for debugging
 * - Inserts into both L2 and L3 caches
 *
 * Returns: 0 on success, -EINVAL on invalid parameters, -ENOMEM on allocation failure
 */
int store_result(u64 fingerprint, const void *data, u32 len, struct mm_struct *mm)
{
	struct cpu_cache *cache;
	struct kcr_entry *entry;
	u32 result_words;

	if (!data || len == 0 || len > KCR_RESULT_SIZE_MAX)
		return -EINVAL;

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return -ENOMEM;

	result_words = (len + sizeof(u64) - 1) / sizeof(u64);
	
	entry->fingerprint = fingerprint;
	memcpy(entry->result, data, len);
	entry->expiry_jiffies = jiffies + msecs_to_jiffies(1000);
	entry->fragment_length = 0;
	entry->register_mask = KCR_MASK_RAX;
	entry->flags = KCR_FLAG_VALID;
	entry->hit_count = 0;
	entry->mm = mm;
	entry->mm_generation = mm ? mm->kcr_generation : 0;
	entry->pid = current->pid;
	entry->pid_ns = task_active_pid_ns(current);

	cache = this_cpu_ptr(&caches);
	store_in_l2(cache, entry, fingerprint);
	store_in_l3(entry, fingerprint);

	return 0;
}
EXPORT_SYMBOL(store_result);

/**
 * invalidate_range() - Invalidate all cache entries for a memory space
 * @mm: Memory space to invalidate
 * @start: Start address of modified range (unused, kept for API compatibility)
 * @end: End address of modified range (unused, kept for API compatibility)
 *
 * Flushes all cache entries associated with given mm_struct.
 * Called by IOMMU notifier on DMA writes or MMU on page modifications.
 * Iterates through all L2 and L3 buckets, removing matching entries.
 * Uses RCU-safe deletion with deferred freeing.
 *
 * Note: Currently invalidates entire mm, not just specified range.
 * Future optimization could implement range-based invalidation.
 */
void invalidate_range(struct mm_struct *mm, unsigned long start, unsigned long end)
{
	struct cpu_cache *cache;
	struct l3_table *table;
	struct kcr_entry *entry, *tmp;
	unsigned long flags;
	int i, socket;

	cache = this_cpu_ptr(&caches);
	for (i = 0; i < KCR_L2_ENTRIES; i++) {
		struct l2_bucket *bkt = &cache->l2[i];
		
		spin_lock_irqsave(&bkt->lock, flags);
		hlist_for_each_entry_safe(entry, tmp, &bkt->head, node) {
			if (entry->mm == mm) {
				hlist_del_rcu(&entry->node);
				call_rcu(&entry->node, free_entry_rcu);
				this_cpu_inc(cache->stats.invalidations);
			}
		}
		spin_unlock_irqrestore(&bkt->lock, flags);
	}

	socket = cpu_to_node(smp_processor_id());
	if (socket >= num_sockets)
		socket = 0;
	
	table = &l3_tables[socket];
	for (i = 0; i < KCR_L3_ENTRIES; i++) {
		struct l2_bucket *bkt = &table->buckets[i];
		
		spin_lock_irqsave(&bkt->lock, flags);
		hlist_for_each_entry_safe(entry, tmp, &bkt->head, node) {
			if (entry->mm == mm) {
				hlist_del_rcu(&entry->node);
				call_rcu(&entry->node, free_entry_rcu);
			}
		}
		spin_unlock_irqrestore(&bkt->lock, flags);
	}

	pr_debug("KCR: invalidated range [%lx-%lx] for mm %p\n", start, end, mm);
}
EXPORT_SYMBOL(invalidate_range);
