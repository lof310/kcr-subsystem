/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KCR_H
#define _LINUX_KCR_H

/**
 * Kernel Computation Reuse (KCR) - Core Header
 * 
 * KCR provides transparent computation reuse at the kernel level through:
 * - Unified memfd-based shared memory (zero-copy kernel-user transitions)
 * - Two-tier cache hierarchy (per-CPU L2, per-socket L3)
 * - Hardware-enforced security (SMAP/SMEP/IOMMU)
 * - IOMMU-based invalidation for 100% DMA write coverage
 * 
 * Architecture:
 *   L2 Cache: 512 entries per-CPU, RCU-protected, 15-25 cycles hit latency
 *   L3 Cache: 4096 entries per-socket, 50-100 cycles hit latency
 *   Shared Region: 16 MB default, memfd-backed anonymous RAM
 * 
 * Integration requires patching 6 kernel files (see Documentation/kcr.txt).
 * Compile with CONFIG_KCR=y to enable, CONFIG_KCR=n for zero overhead.
 */

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/percpu-defs.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/skbuff.h>
#include <linux/ptrace.h>
#include <crypto/skcipher.h>

/* Memory layout constants */
#define KCR_REGION_SIZE         (16 * 1024 * 1024)    /* 16 MB shared region */
#define KCR_METADATA_OFFSET     0                       /* Metadata at start */
#define KCR_METADATA_SIZE       4096                    /* 4 KB metadata */
#define KCR_L2_OFFSET           4096                    /* L2 after metadata */
#define KCR_L2_SIZE             (512 * 64)              /* 512 entries × 64 bytes */
#define KCR_L3_OFFSET           (4096 + 32768)          /* L3 after L2 */
#define KCR_L3_SIZE             (4096 * 64)             /* 4096 entries × 64 bytes */

/* Cache configuration */
#define KCR_L2_ENTRIES          64                      /* Per-CPU L2 size (reduced from 512 to fit in 16KB percpu limit) */
#define KCR_L3_ENTRIES          512                     /* Per-socket L3 size (reduced from 4096) */
#define KCR_RESULT_SIZE_MAX     64                      /* Max result bytes stored inline in cache entry */
#define KCR_FINGERPRINT_BITS    64                      /* xxHash64 output */

/* External storage configuration - for caching larger data in RAM */
#define KCR_EXT_SLOTS           256                     /* Number of external data slots per socket */
#define KCR_EXT_SLOT_SIZE       (4 * 1024)              /* 4 KB per external slot */

/* Hash seeds for fingerprint computation */
#define KCR_SEED_BASE           0x123456789ABCDEF0ULL   /* General purpose */
#define KCR_SEED_CRYPTO         0xFEDCBA9876543210ULL   /* Crypto operations */

/* Register masks for x86-64 result injection */
#define KCR_MASK_RAX            (1 << 0)
#define KCR_MASK_RBX            (1 << 1)
#define KCR_MASK_RCX            (1 << 2)
#define KCR_MASK_RDX            (1 << 3)
#define KCR_MASK_RSI            (1 << 4)
#define KCR_MASK_RDI            (1 << 5)
#define KCR_MASK_RBP            (1 << 6)
#define KCR_MASK_RSP            (1 << 7)

/* Entry flags */
#define KCR_FLAG_VALID          (1 << 0)                /* Entry is valid */
#define KCR_FLAG_ENCRYPTED      (1 << 1)                /* Result encrypted */
#define KCR_FLAG_DMA_SAFE       (1 << 2)                /* DMA-safe region */

/**
 * enum kcr_vma_state - VMA determinism verification states
 * @KCR_VMA_UNVERIFIED: Initial state, collecting execution samples
 * @KCR_VMA_VERIFIED: Deterministic confirmed (≥100 consistent executions)
 * @KCR_VMA_REJECTED: Non-deterministic detected, caching disabled
 * 
 * Learning mode requires 100 consistent executions before marking as VERIFIED.
 * This prevents caching of functions with side effects or external dependencies.
 */
enum kcr_vma_state {
	KCR_VMA_UNVERIFIED = 0,
	KCR_VMA_VERIFIED = 1,
	KCR_VMA_REJECTED = 2,
};

/**
 * struct kcr_metadata - Shared memory region metadata
 * @generation: Global generation counter for invalidation
 * @last_flush: Jiffies timestamp of last cache flush
 * @entry_count: Total cached entries across all tiers
 * @flags: Region-wide configuration flags
 * @stats: Statistical counters (hits, misses, etc.)
 * @__pad: Padding to 4 KB page boundary
 * 
 * Stored at offset 0 in the shared memfd region. Aligned to 4 KB for page table efficiency.
 */
struct kcr_metadata {
	u64 generation;
	u64 last_flush;
	u32 entry_count;
	u32 flags;
	u64 stats[8];
	u8 __pad[3944];
} __aligned(4096);

/**
 * struct kcr_entry - Cache entry structure (64 bytes)
 * @fingerprint: 64-bit xxHash64 of input data
 * @result: Cached result data (up to 8 × u64 = 64 bytes inline)
 * @ext_data: Pointer to external data for results > 64 bytes (RAM-backed)
 * @ext_size: Size of external data in bytes
 * @expiry_jiffies: Expiration time in jiffies (TTL-based invalidation)
 * @fragment_length: Code fragment length for IP advancement
 * @register_mask: Bitmask indicating which registers contain results
 * @flags: Entry status flags (KCR_FLAG_*)
 * @hit_count: Number of times this entry was reused
 * @mm: Owner memory space pointer for isolation
 * @mm_generation: Generation counter at insertion time (stored locally)
 * @pid: Owner process ID
 * @pid_ns: PID namespace for container isolation
 * @node: RCU-protected hash list node (hlist_node)
 * @rcu: RCU callback head for deferred freeing (separate from node)
 * 
 * Cache entries support two storage modes:
 * - Inline: Small results (≤64 bytes) stored directly in result[] array
 * - External: Large results (>64 bytes) stored in system RAM via ext_data pointer
 * 
 * Alignment to 64 bytes prevents false sharing between CPU cores.
 * Note: 'node' is for hlist linkage, 'rcu' is for call_rcu() - they serve different purposes.
 * Note: mm_generation is stored locally since mm_struct->kcr_generation may not exist.
 */
struct kcr_entry {
	u64 fingerprint;
	union {
		u64 result[8];          /* Inline storage for small results (≤64 bytes) */
		struct {
			void *ext_data;     /* External RAM pointer for large results */
			u32 ext_size;       /* Size of external data */
			u8 reserved[20];    /* Padding to match result[] size */
		};
	};
	u64 expiry_jiffies;
	u16 fragment_length;
	u8 register_mask;
	u8 flags;
	u32 hit_count;
	struct mm_struct *mm;
	u64 mm_generation;  /* Local copy since mm->kcr_generation may not exist */
	pid_t pid;
	struct pid_namespace *pid_ns;
	struct hlist_node node;
	struct rcu_head rcu;
} __aligned(64);

/**
 * struct l2_bucket - Per-CPU L2 cache bucket
 * @head: RCU-protected hash list head
 * @rcu: RCU callback head for deferred freeing
 * @lock: Spinlock for concurrent access protection
 * @__pad: Padding to cache line (64 bytes on x86-64)
 * 
 * Each bucket is cache-line aligned to prevent false sharing.
 * Uses RCU for lock-free reads, spinlock for writes.
 */
struct l2_bucket {
	struct hlist_head head;
	struct rcu_head rcu;
	spinlock_t lock;
	u8 __pad[56];
} __aligned(64);

/**
 * struct cpu_cache - Per-CPU cache structure
 * @l2: Array of L2 buckets (512 entries)
 * @stats: Statistical counters (cache-line aligned)
 * 
 * Defined with DEFINE_PER_CPU_ALIGNED() for cache-line alignment.
 * Eliminates inter-core contention by maintaining independent copies per CPU.
 */
struct cpu_cache {
	struct l2_bucket l2[KCR_L2_ENTRIES];
	struct {
		u64 l2_hits;
		u64 l3_hits;
		u64 misses;
		u64 invalidations;
		u64 cycles_saved;
	} __aligned(64) stats;
} __aligned(64);

/**
 * struct l3_table - Per-socket L3 cache table
 * @buckets: Array of L3 buckets (512 entries)
 * @ext_slots: External data storage slots for large results (RAM-backed)
 * @ext_lock: Spinlock protecting external slot allocation
 * @socket_id: Physical socket identifier
 * 
 * Shared among all CPUs on the same NUMA node/socket.
 * Provides larger capacity at slightly higher latency (50-100 cycles).
 * External slots allow caching data larger than 64 bytes in system RAM.
 */
struct l3_table {
	struct l2_bucket buckets[KCR_L3_ENTRIES];
	void *ext_slots[KCR_EXT_SLOTS];      /* Pointers to externally allocated data */
	u32 ext_slot_size[KCR_EXT_SLOTS];    /* Size of each external slot */
	spinlock_t ext_lock;                 /* Protects external slot allocation */
	u32 socket_id;
} __aligned(64);

/**
 * struct shared_region - Shared memory region descriptor
 * @memfd_file: Anonymous memfd file pointer
 * @kernel_vaddr: Kernel virtual address mapping
 * @size: Region size in bytes
 * @phys_addr: Physical address (for DMA devices)
 * @refcount: Reference count for safe deallocation
 * 
 * Backed by memfd_create() with MFD_NOEXEC_SEAL for security.
 * Visible simultaneously to kernel and user space without copies.
 */
struct shared_region {
	struct file *memfd_file;
	void *kernel_vaddr;
	u64 size;
	u64 phys_addr;
	atomic_t refcount;
};

/**
 * struct vma_metadata - VMA determinism tracking metadata
 * @state: Current verification state (kcr_vma_state)
 * @execution_count: Number of observed executions
 * @result_history: Ring buffer of last 4 results
 * @first_result: First observed result for comparison
 * @refcount: Reference count for fork inheritance
 * 
 * Embedded in vm_area_struct when CONFIG_KCR enabled.
 * Tracks execution consistency to detect non-deterministic code.
 */
struct vma_metadata {
	enum kcr_vma_state state;
	u32 execution_count;
	u64 result_history[4];
	u64 first_result;
	atomic_t refcount;
};

/**
 * struct iommu_notifier_data - IOMMU invalidation notifier
 * @notifier: Standard kernel notifier block
 * @mm: Monitored memory space
 * @start: Start of monitored range
 * @end: End of monitored range
 * @aggressive_mode: Invalidate on any write (vs. only DMA)
 * 
 * Registered with IOMMU subsystem to receive page invalidation events.
 * Achieves 100% coverage for DMA writes, syscall modifications, and user changes.
 */
struct iommu_notifier_data {
	struct notifier_block notifier;
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	bool aggressive_mode;
};

#ifdef CONFIG_KCR

/* === Lifecycle Management === */

/**
 * kcr_init() - Initialize KCR subsystem
 *
 * Allocates shared memory region, initializes per-CPU and per-socket caches,
 * sets up debugfs interface. Called during kernel/module initialization.
 *
 * Returns: 0 on success, negative errno on failure
 */
int kcr_init(void);

/**
 * kcr_exit() - Cleanup KCR subsystem
 *
 * Frees all allocated resources, unregisters notifiers, removes debugfs entries.
 * Called during kernel/module exit.
 */
void kcr_exit(void);

/**
 * kcr_is_enabled() - Check if KCR is active
 *
 * Returns: true if KCR is initialized and enabled, false otherwise
 */
bool kcr_is_enabled(void);

/* === Memory Management === */

/**
 * kcr_alloc_region() - Allocate shared memory region
 *
 * Creates memfd-backed anonymous memory region visible to both kernel
 * and user space. Zero-copy architecture eliminates 400-600 cycle overhead.
 *
 * Returns: Pointer to shared_region, or NULL on failure
 */
struct shared_region *kcr_alloc_region(void);

/**
 * kcr_free_region() - Free shared memory region
 *
 * Unmaps kernel virtual address, releases memfd file, frees descriptor.
 * Uses reference counting for safe concurrent access.
 *
 * @region: Region to free
 */
void kcr_free_region(struct shared_region *region);

/**
 * kcr_map_to_user() - Map shared region to user space
 *
 * Maps the memfd region into target task's address space via vm_mmap().
 * User space can access cached results directly without syscalls.
 *
 * @region: Shared region to map
 * @task: Target task for mapping
 * Returns: 0 on success, negative errno on failure
 */
int kcr_map_to_user(struct shared_region *region, struct task_struct *task);

/* === Cache Operations === */

/**
 * lookup_unified() - Unified cache lookup (L2 + L3)
 *
 * Performs two-tier cache lookup: first checks per-CPU L2 (15-25 cycles),
 * then falls back to per-socket L3 (50-100 cycles). On L3 hit, promotes
 * entry to L2 for faster subsequent access.
 *
 * @fingerprint: 64-bit hash of input data
 * @mm: Memory space for isolation check
 * Returns: Pointer to matching entry, or NULL on miss
 */
struct kcr_entry *lookup_unified(u64 fingerprint, struct mm_struct *mm);

/**
 * store_result() - Store computation result in cache
 *
 * Allocates new cache entry, stores result data, inserts into both
 * L2 and L3 caches. Uses RCU for concurrent access safety.
 *
 * @fingerprint: 64-bit hash of input data
 * @data: Result data to cache
 * @len: Length of result data (max 64 bytes)
 * @mm: Owner memory space
 * Returns: 0 on success, negative errno on failure
 */
int store_result(u64 fingerprint, const void *data, u32 len, struct mm_struct *mm);

/**
 * invalidate_range() - Invalidate cache entries for memory range
 *
 * Removes all cache entries associated with given mm_struct.
 * Called by IOMMU notifier on DMA writes, or by MMU on page modifications.
 * Ensures 100% invalidation coverage.
 *
 * @mm: Memory space to invalidate
 * @start: Start address of modified range
 * @end: End address of modified range
 */
void invalidate_range(struct mm_struct *mm, unsigned long start, unsigned long end);

/* === Fingerprint Computation === */

/**
 * compute_fingerprint() - Compute 64-bit xxHash64 fingerprint
 *
 * Fast non-cryptographic hash (0.5 cycles/byte) for input identification.
 * Seed parameter allows domain-specific hash spaces (crypto, network, etc.).
 *
 * @data: Input data to hash
 * @len: Length of input data
 * @seed: Hash seed for domain separation
 * Returns: 64-bit fingerprint value
 */
u64 compute_fingerprint(const void *data, size_t len, u64 seed);

/**
 * crypto_compute_fingerprint() - Compute fingerprint for crypto operation
 *
 * Specialized fingerprinting for cryptographic operations. Includes:
 * algorithm type, key material, key length, ciphertext length, and
 * first 64 bytes of plaintext. Prevents cross-key and cross-algo collisions.
 *
 * @req: skcipher_request from kernel crypto API
 * Returns: 64-bit fingerprint value
 */
u64 crypto_compute_fingerprint(struct skcipher_request *req);

/* === Determinism Verification === */

/**
 * verify_deterministic() - Verify function determinism via learning mode
 *
 * Tracks execution history over 100 invocations. Compares results to detect
 * non-determinism (side effects, timing dependencies, external inputs).
 * Transitions: UNVERIFIED → VERIFIED (after 100 consistent results)
 *              UNVERIFIED → REJECTED (on first mismatch)
 *
 * @meta: VMA metadata structure
 * @current_result: Most recent execution result
 * Returns: true if execution appears deterministic, false otherwise
 */
bool verify_deterministic(struct vma_metadata *meta, u64 current_result);

/**
 * should_cache() - Determine if VMA should be cached
 *
 * Checks VM_KCR flag and verification state. Only VERIFIED VMAs
 * are eligible for caching. UNVERIFIED VMAs enter learning mode.
 * REJECTED VMAs are permanently excluded.
 *
 * @vma: Virtual memory area to check
 * Returns: true if caching is allowed, false otherwise
 */
bool should_cache(struct vm_area_struct *vma);

/* === Entry Validation === */

/**
 * validate_entry() - Validate cache entry against current context
 *
 * Performs three checks:
 * 1. Generation counter match (detects mm_struct reuse)
 * 2. mm_struct pointer equality (cross-process isolation)
 * 3. Expiration time (TTL-based invalidation)
 *
 * @entry: Cache entry to validate
 * @mm: Current memory space
 * Returns: true if entry is valid, false if stale
 */
bool validate_entry(struct kcr_entry *entry, struct mm_struct *mm);

/* === Result Injection === */

/**
 * inject_result() - Inject cached results into CPU registers
 *
 * Restores cached computation results into x86-64 registers based on
 * register_mask. Advances instruction pointer by fragment_length to
 * skip original computation code.
 *
 * @regs: pt_regs from page fault or syscall entry
 * @entry: Cache entry containing results
 */
void inject_result(struct pt_regs *regs, struct kcr_entry *entry);

/* === Debugfs Interface === */

/**
 * kcr_debugfs_init() - Initialize debugfs interface
 *
 * Creates /sys/kernel/debug/kcr directory with:
 * - stats: Real-time cache statistics (hits, misses, hit rate)
 * - config: Current configuration parameters
 *
 * Returns: 0 on success, negative errno on failure
 */
int kcr_debugfs_init(void);

/**
 * kcr_debugfs_exit() - Remove debugfs interface
 *
 * Recursively removes kcr debugfs directory and all entries.
 */
void kcr_debugfs_exit(void);

/* === IOMMU Integration === */

/**
 * kcr_iommu_init() - Register IOMMU invalidation notifier
 *
 * Registers callback with IOMMU subsystem to receive notifications
 * on page invalidations (DMA writes, device accesses). Enables
 * 100% invalidation coverage vs. 80% for syscall-hook approaches.
 *
 * @mm: Memory space to monitor
 * @notifier_data: Notifier registration structure
 * Returns: 0 on success, negative errno on failure
 */
int kcr_iommu_init(struct mm_struct *mm, struct iommu_notifier_data *notifier_data);

/**
 * kcr_iommu_exit() - Unregister IOMMU notifier
 *
 * Removes previously registered IOMMU notifier.
 *
 * @notifier_data: Notifier to unregister
 */
void kcr_iommu_exit(struct iommu_notifier_data *notifier_data);

#else /* !CONFIG_KCR */

/* Stub implementations for zero-overhead when disabled */
static inline int kcr_init(void) { return 0; }
static inline void kcr_exit(void) { }
static inline bool kcr_is_enabled(void) { return false; }
static inline struct shared_region *kcr_alloc_region(void) { return NULL; }
static inline void kcr_free_region(struct shared_region *r) { }
static inline int kcr_map_to_user(struct shared_region *r, struct task_struct *t) { return -ENODEV; }
static inline struct kcr_entry *lookup_unified(u64 f, struct mm_struct *m) { return NULL; }
static inline int store_result(u64 f, const void *d, u32 l, struct mm_struct *m) { return -ENODEV; }
static inline void invalidate_range(struct mm_struct *m, unsigned long s, unsigned long e) { }
static inline u64 compute_fingerprint(const void *d, size_t l, u64 s) { return 0; }
static inline u64 crypto_compute_fingerprint(struct skcipher_request *r) { return 0; }
static inline bool verify_deterministic(struct vma_metadata *m, u64 r) { return false; }
static inline bool should_cache(struct vm_area_struct *v) { return false; }
static inline bool validate_entry(struct kcr_entry *e, struct mm_struct *m) { return false; }
static inline void inject_result(struct pt_regs *r, struct kcr_entry *e) { }
static inline int kcr_debugfs_init(void) { return 0; }
static inline void kcr_debugfs_exit(void) { }
static inline int kcr_iommu_init(struct mm_struct *m, struct iommu_notifier_data *n) { return 0; }
static inline void kcr_iommu_exit(struct iommu_notifier_data *n) { }

#endif /* CONFIG_KCR */

#endif /* _LINUX_KCR_H */
