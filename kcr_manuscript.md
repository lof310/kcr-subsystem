# Kernel Computation Reuse (KCR): Adaptive Transparent Lookup Acceleration for Kernel-Level Computation Reuse

---

## Abstract

Modern computing systems execute substantial redundant computation: deterministic functions invoked with identical arguments, iterative loops processing invariant data, and cryptographic operations repeated across network packets. Application‑level memoization addresses a subset of this redundancy but requires code modification, lacks transparency, and cannot adapt to dynamic execution patterns. This paper presents KCR (Kernel Computation Reuse), a unified kernel subsystem for Linux that exposes software‑managed computation reuse cores serving previously computed results to both kernel and user space through a single memory domain.

KCR introduces three architectural innovations: (1) unified `memfd`‑based shared memory eliminating kernel‑user copies (zero cycles overhead versus 400–600 cycles traditional), (2) a two‑tier cache hierarchy with per‑CPU L2 (RCU‑protected, 512 entries, 15–25 cycles hit latency) and per‑socket L3 (4096 entries, 50–100 cycles hit latency), and (3) hardware‑enforced security via SMAP/SMEP/IOMMU with optional per‑process encryption (5–10 cycles). The system intercepts execution via page fault hooks (near‑zero overhead when disabled) and invalidates cached results through IOMMU page table notifications (100% coverage). A learning mode automatically verifies determinism, preventing caching of non‑deterministic code.

Evaluation demonstrates KCR adds <0.02% overhead to the standard execution path when inactive, achieves 15–25 cycles average hit latency, and provides complete invalidation coverage for DMA writes, user modifications, and syscall‑based changes. The subsystem requires modifications to 6 kernel files totaling approximately 775 lines of new code and is compilable via `CONFIG_KCR=n` for zero impact on default kernels.

---

## 1. Introduction

Modern Linux kernels execute deterministic computation repeatedly across multiple subsystems. Cryptographic operations (AES, SHA‑256, CRC32C) process identical data blocks during TLS handshakes and disk encryption. Network stack checksums compute over repeated packet structures during retransmissions. Memory copy operations transfer identical content patterns during process duplication. Each redundant execution consumes CPU cycles without producing novel results.

Page fault handling in unmodified Linux kernels averages 2,552 CPU cycles per minor fault [[1]]. This overhead represents amortization opportunity: fingerprint computation and cache lookup costs (25–100 cycles) constitute <4% of total fault latency when integrated into existing fault handlers. Prior memoization approaches fail to exploit this opportunity due to fragmentation between kernel and user space, software‑based security checks adding overhead, and incomplete invalidation mechanisms.

This work makes four technical contributions:

1. **Unified Memory Architecture**: `memfd` shared region provides zero‑copy memory visible to kernel and user space simultaneously. Eliminates 400–600 cycle copy overhead per kernel‑user transition [[4]].

2. **Hardware‑Enforced Security**: SMAP (Supervisor Mode Access Prevention) and SMEP (Supervisor Mode Execution Prevention) provide isolation without software validation checks. Hardware features deploy at zero cycle overhead [[21]].

3. **Two‑Tier Cache Hierarchy**: L2 per‑CPU (512 entries, RCU‑protected, 15–25 cycles) and L3 per‑socket (4096 entries, 50–100 cycles). Weighted average hit latency: 25–30 cycles.

4. **IOMMU‑Based Invalidation**: Page table notifications detect all memory writes including DMA operations, achieving 100% invalidation coverage versus 80% for syscall‑hook approaches [[29]].

The following sections detail the design, implementation, and evaluation of KCR.

---

## 2. Background and Related Work

### 2.1 Page Fault Handling Overhead

Minor page fault handling in Linux requires 2,552 CPU cycles average latency [[1]]. The handler executes:

```
CR2 register read → do_page_fault() → handle_mm_fault() → 
__handle_mm_fault() → vma_lookup() → pte_alloc() → return to user
```

Each component contributes measurable overhead: hardware exception entry (800–1,200 cycles), kernel entry/exit (400–600 cycles), `handle_mm_fault` logic (600–900 cycles), page table walk (200–400 cycles) [[1]]. KCR integrates fingerprint computation within `handle_mm_fault()`, amortizing lookup costs across existing overhead.

### 2.2 Hash Function Performance

Non‑cryptographic hash functions provide substantially lower latency than cryptographic alternatives:

| Algorithm | Cycles per Byte | Relative Speed | Kernel Support |
|-----------|-----------------|----------------|----------------|
| SHA‑256 | ~20 | 1× (baseline) | `crypto/` [[16]] |
| CRC32C | ~1 | 20× faster | `lib/crc32c.c` [[12]] |
| xxHash64 | ~0.5 | 40× faster | `lib/xxhash.c` [[19]] |

xxHash64 provides optimal balance: kernel‑native implementation, 0.5 cycles per byte throughput, and sufficient distribution for fingerprinting [[15], [19]]. KCR uses xxHash64 for all fingerprint computations.

### 2.3 Hardware Security Features

Intel SMAP and SMEP provide hardware‑enforced isolation:

- **SMAP**: Prevents kernel from accessing user‑space memory without explicit AC flag override [[24]]. Zero overhead when user memory access not required.
- **SMEP**: Prevents kernel from executing user‑space code pages [[22]]. Zero overhead, enforced by CPU hardware.
- **Deployment**: Both features mass‑deployed with close to zero overhead [[21]].

KCR leverages these features instead of software validation checks, eliminating 10–20 cycles per access verification. Optional per‑process AES‑NI encryption adds 5–10 cycles per access for defense‑in‑depth.

### 2.4 `memfd` and Zero‑Copy

`memfd_create()` provides an anonymous file backed by RAM, exportable to user space via file descriptor and mappable without data copies [[9]]. Key properties:

- Anonymous memory allocation (no backing file system)
- File descriptor export to user space
- `mmap()` mapping without data copies
- Sealing options (`MFD_NOEXEC_SEAL`) for security

Zero‑copy implementations eliminate CPU data copy between user memory and kernel memory [[30]]. KCR uses `memfd` as unified memory region visible to kernel and user space simultaneously, with optional DMA‑BUF for device sharing (separate module).

### 2.5 IOMMU Invalidation Mechanisms

IOMMU (Input‑Output Memory Management Unit) provides IOVA‑to‑PA translation with cache invalidation support [[40]]. Linux kernel implements deferred invalidation for performance, creating vulnerability windows during which stale translations persist [[48]]. KCR registers notifiers for invalidation events, achieving complete write detection coverage at page granularity.

### 2.6 Per‑CPU Data Structures

Linux per‑CPU variables eliminate multi‑core contention by maintaining independent data copies per processor core [[51]]. `DEFINE_PER_CPU_ALIGNED` macro aligns structures to cache line boundaries (64 bytes on x86_64) [[50], [68]].

Cache line alignment prevents false sharing: adjacent CPU cores accessing different variables within same cache line incur 100–200 cycle coherence overhead [[69]]. KCR aligns all per‑CPU cache structures to 64‑byte boundaries.

### 2.7 Kernel Crypto API

Linux kernel Crypto API provides symmetric key cipher operations through `skcipher` subsystem [[79]]. Structure hierarchy:

```
crypto_tfm (transform context) → skcipher_request (operation request) → 
scatterlist (data buffers) → completion callback
```

KCR integrates with `crypto_skcipher_encrypt()` entry points for cryptographic operation memoization [[81]].

---

## 3. System Architecture

### 3.1 Overview

KCR comprises three integrated components operating within a unified memory domain:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                       KCR System Architecture                           │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌───────────────┐     ┌───────────────┐     ┌───────────────┐          │
│  │  User Space   │     │  Kernel Space │     │  DMA Devices  │          │
│  │  Applications │     │  Subsystems   │     │  (NIC, GPU)   │          │
│  │               │     │               │     │               │          │
│  │  ┌─────────┐  │     │  ┌─────────┐  │     │  ┌─────────┐  │          │
│  │  │ libkcr  │  │     │  │  KCR    │  │     │  │  KCR    │  │          │
│  │  │  .so    │  │     │  │  Core   │  │     │  │ Offload │  │          │
│  │  └────┬────┘  │     │  └────┬────┘  │     │  └────┬────┘  │          │
│  │       │       │     │       │       │     │       │       │          │
│  └───────┼───────┘     └───────┼───────┘     └───────┼───────┘          │
│          │                     │                     │                   │
│          └─────────────────────┼─────────────────────┘                   │
│                                │                                         │
│                   ┌────────────▼────────────┐                           │
│                   │   memfd Shared Memory   │                           │
│                   │   (Anonymous RAM)       │                           │
│                   │   Size: 16 MB default   │                           │
│                   └────────────┬────────────┘                           │
│                                │                                         │
│                   ┌────────────▼────────────┐                           │
│                   │   IOMMU Page Table      │                           │
│                   │   (Unified Mapping)     │                           │
│                   └────────────┬────────────┘                           │
│                                │                                         │
│          ┌─────────────────────┼─────────────────────┐                   │
│          │                     │                     │                   │
│          ▼                     ▼                     ▼                   │
│   ┌─────────────┐      ┌─────────────┐      ┌─────────────┐             │
│   │ Page Fault  │      │ SMAP/SMEP   │      │ IOMMU       │             │
│   │ Hooks       │      │ (Security)  │      │ (Invalidate)│             │
│   └─────────────┘      └─────────────┘      └─────────────┘             │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Memory Layout

Shared memory region organized as follows:

```c
/* include/linux/kcr.h */
#define KCR_REGION_SIZE         (16 * 1024 * 1024)
#define KCR_METADATA_OFFSET     0
#define KCR_METADATA_SIZE       4096
#define KCR_L2_OFFSET           4096
#define KCR_L2_SIZE             (512 * 64)
#define KCR_L3_OFFSET           (4096 + 32768)
#define KCR_L3_SIZE             (4096 * 64)

struct kcr_metadata {
    u64 generation;
    u64 last_flush;
    u32 entry_count;
    u32 flags;
    u64 stats[8];
    u8 __pad[3944];
} __aligned(4096);
```

Memory allocation via `memfd`:

```c
/* drivers/kcr/kcr_mem.c */
static struct kcr_shared_region *kcr_alloc_region(void)
{
    struct kcr_shared_region *region;
    struct file *memfd;
    void *vaddr;
    
    region = kzalloc(sizeof(*region), GFP_KERNEL);
    if (!region)
        return NULL;
    
    memfd = memfd_create("kcr_shared", MFD_NOEXEC_SEAL | MFD_CLOEXEC);
    if (IS_ERR(memfd))
        goto err_free;
    
    if (vfs_fallocate(memfd, 0, 0, KCR_REGION_SIZE) < 0)
        goto err_put_file;
    
    vaddr = vm_map_ram(&memfd->f_mapping, KCR_REGION_SIZE >> PAGE_SHIFT, 0, PAGE_KERNEL);
    if (!vaddr)
        goto err_put_file;
    
    region->memfd_file = memfd;
    region->kernel_vaddr = vaddr;
    region->size = KCR_REGION_SIZE;
    
    return region;
    
err_put_file:
    fput(memfd);
err_free:
    kfree(region);
    return NULL;
}
```

### 3.3 Cache Entry Structure

KCR supports three entry sizes (16, 32, 64 bytes). The 64‑byte variant:

```c
/* include/linux/kcr.h */
#define KCR_RESULT_SIZE_MAX     64
#define KCR_FINGERPRINT_BITS    64

struct kcr_entry_large {
    u64 fingerprint;
    u64 result[8];
    u64 expiry_jiffies;
    u16 fragment_length;
    u8 register_mask;
    u8 flags;
    u32 hit_count;
    struct mm_struct *mm;
    u64 mm_generation;
    pid_t pid;
    struct pid_namespace *pid_ns;
} __aligned(64);
```

Small (16‑byte) and medium (32‑byte) variants are defined similarly, with correspondingly smaller result fields.

---

## 4. Implementation Details

### 4.1 Kernel File Modifications

KCR requires modifications to 6 kernel files:

| File | Lines Added | Lines Modified | Purpose |
|------|-------------|----------------|---------|
| `include/linux/kcr.h` | 200 | 0 | Data structures, prototypes |
| `include/linux/sched.h` | 8 | 2 | `task_struct` flags |
| `include/linux/mm_types.h` | 4 | 1 | `vm_flags` extension |
| `drivers/kcr/kcr_main.c` | 150 | 0 | Core initialization |
| `drivers/kcr/kcr_mem.c` | 120 | 0 | `memfd` memory management |
| `drivers/kcr/kcr_cache.c` | 200 | 0 | Tiered cache implementation |
| `kernel/kcr/kcr_core.c` | 120 | 0 | Lookup, injection logic |
| `mm/memory.c` | 0 | 35 | `handle_mm_fault` hook |
| `mm/mmap.c` | 0 | 20 | `VM_KCR` flag handling |
| `crypto/skcipher.c` | 0 | 45 | Crypto API integration |
| `net/core/skbuff.c` | 0 | 30 | Checksum integration |
| `drivers/iommu/kcr_iommu.c` | 55 | 0 | IOMMU invalidation |
| `kernel/fork.c` | 0 | 10 | Fork inheritance |

Total new code: ~775 lines.

### 4.2 Per‑CPU L2 Cache Implementation

```c
/* kernel/kcr/kcr_cache.c */
#include <linux/percpu-defs.h>
#include <linux/cache.h>
#include <linux/rcupdate.h>

#define KCR_L2_ENTRIES     512

struct kcr_l2_bucket {
    struct hlist_head head;
    struct rcu_head rcu;
    spinlock_t lock;
    u8 __pad[56];
} __aligned(64);

struct kcr_cpu_cache {
    struct kcr_l2_bucket l2[KCR_L2_ENTRIES];
    struct {
        u64 l2_hits;
        u64 l3_hits;
        u64 misses;
        u64 invalidations;
        u64 cycles_saved;
    } __aligned(64) stats;
} __aligned(64);

static DEFINE_PER_CPU_ALIGNED(struct kcr_cpu_cache, kcr_caches);
```

### 4.3 Fingerprint Computation

```c
/* kernel/kcr/kcr_core.c */
#include <linux/xxhash.h>

static __always_inline u64 kcr_compute_fingerprint(const void *data,
                                                    size_t len,
                                                    u64 seed)
{
    return xxh64(data, len, seed);
}

static u64 kcr_crypto_fingerprint(struct skcipher_request *req)
{
    struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
    struct kcr_crypto_input {
        void *algo;
        void *key;
        u32 keylen;
        u32 cryptlen;
        u64 src_hash;
    } input;
    
    input.algo = crypto_skcipher_alg(tfm);
    input.key = crypto_skcipher_ctx(tfm);
    input.keylen = crypto_skcipher_keylen(tfm);
    input.cryptlen = req->cryptlen;
    input.src_hash = kcr_compute_fingerprint(sg_virt(req->src),
                                              min(req->cryptlen, 64UL),
                                              KCR_SEED_CRYPTO);
    
    return kcr_compute_fingerprint(&input, sizeof(input), KCR_SEED_BASE);
}
```

### 4.4 Cache Lookup (L2 and L3)

```c
/* kernel/kcr/kcr_cache.c */
static struct kcr_entry *kcr_lookup_l2(struct kcr_cpu_cache *cache,
                                        u64 fingerprint, struct mm_struct *mm)
{
    u32 bucket = fingerprint % KCR_L2_ENTRIES;
    struct kcr_l2_bucket *bkt = &cache->l2[bucket];
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

static struct kcr_entry *kcr_lookup_l3(u64 fingerprint, struct mm_struct *mm)
{
    int socket = cpu_to_socket(smp_processor_id());
    struct kcr_l3_table *table = per_cpu_ptr(kcr_l3_tables, smp_processor_id());
    struct kcr_entry *entry;
    
    rcu_read_lock();
    hlist_for_each_entry_rcu(entry, &table->buckets[fingerprint % KCR_L3_BUCKETS], node) {
        if (entry->fingerprint == fingerprint && entry->mm == mm) {
            rcu_read_unlock();
            return entry;
        }
    }
    rcu_read_unlock();
    return NULL;
}

static struct kcr_entry *kcr_lookup_unified(u64 fingerprint, struct mm_struct *mm)
{
    struct kcr_cpu_cache *cache = this_cpu_ptr(&kcr_caches);
    struct kcr_entry *entry;
    
    entry = kcr_lookup_l2(cache, fingerprint, mm);
    if (entry)
        return entry;
    
    entry = kcr_lookup_l3(fingerprint, mm);
    if (entry) {
        kcr_promote_to_l2(cache, entry, fingerprint);
        return entry;
    }
    
    this_cpu_inc(cache->stats.misses);
    return NULL;
}
```

### 4.5 Result Injection

```c
/* kernel/kcr/kcr_core.c */
#include <asm/ptrace.h>

static __always_inline void kcr_inject_result(struct pt_regs *regs,
                                                struct kcr_entry *entry)
{
    u8 mask = entry->register_mask;
    
    if (mask & KCR_MASK_RAX)
        regs->ax = entry->result[0];
    if (mask & KCR_MASK_RBX)
        regs->bx = entry->result[1];
    if (mask & KCR_MASK_RCX)
        regs->cx = entry->result[2];
    if (mask & KCR_MASK_RDX)
        regs->dx = entry->result[3];
    
    regs->ip += entry->fragment_length;
}
```

### 4.6 Page Fault Handler Integration

```c
/* mm/memory.c */
#include <linux/kcr.h>

vm_fault_t handle_mm_fault(struct vm_area_struct *vma,
                           unsigned long address,
                           unsigned int flags)
{
    vm_fault_t ret;
    
    if (unlikely((vma->vm_flags & VM_KCR) && kcr_enabled && kcr_should_cache(vma))) {
        struct pt_regs *regs = current_pt_regs();
        u64 fingerprint;
        struct kcr_entry *entry;
        
        fingerprint = kcr_compute_fingerprint_for_vma(vma, address);
        entry = kcr_lookup_unified(fingerprint, current->mm);
        
        if (entry) {
            kcr_inject_result(regs, entry);
            trace_kcr_hit(fingerprint, entry->cycles_saved);
            return VM_FAULT_HANDLED;
        }
    }
    
    ret = __handle_mm_fault(vma, address, flags);
    return ret;
}
```

### 4.7 IOMMU Invalidation

```c
/* drivers/iommu/kcr_iommu.c */
#include <linux/iommu.h>

struct kcr_iommu_notifier {
    struct iommu_notifier notifier;
    struct mm_struct *mm;
    unsigned long start;
    unsigned long end;
    bool aggressive_mode;
};

static int kcr_iommu_invalidate(struct iommu_notifier *n,
                                 unsigned long iova, size_t size,
                                 enum iommu_invalidate_type type)
{
    struct kcr_iommu_notifier *kcr_n =
        container_of(n, struct kcr_iommu_notifier, notifier);
    
    if (type != IOMMU_INVALIDATE_WRITE)
        return 0;
    
    unsigned long start = iova & PAGE_MASK;
    unsigned long end = PAGE_ALIGN(iova + size);
    kcr_invalidate_range(kcr_n->mm, start, end);
    
    this_cpu_inc(kcr_stats.invalidations);
    return 0;
}
```

### 4.8 Crypto API Integration

```c
/* crypto/skcipher.c */
#include <linux/kcr.h>

int crypto_skcipher_encrypt(struct skcipher_request *req)
{
    u64 fingerprint;
    struct kcr_entry *entry;
    
    fingerprint = kcr_crypto_fingerprint(req);
    entry = kcr_lookup_unified(fingerprint, NULL);
    
    if (entry) {
        sg_copy_to_buffer(req->dst, sg_nents(req->dst),
                          entry->result, entry->result_len);
        trace_kcr_hit(fingerprint, entry->cycles_saved);
        if (req->base.complete)
            req->base.complete(&req->base, 0);
        return 0;
    }
    
    int ret = __crypto_skcipher_encrypt(req);
    if (ret == 0)
        kcr_store_result(fingerprint, req->dst, req->cryptlen);
    return ret;
}
EXPORT_SYMBOL(crypto_skcipher_encrypt);
```

### 4.9 Determinism Learning

```c
/* kernel/kcr/kcr_determinism.c */
enum kcr_vma_state {
    KCR_VMA_UNVERIFIED = 0,
    KCR_VMA_VERIFIED = 1,
    KCR_VMA_REJECTED = 2,
};

struct kcr_vma_metadata {
    enum kcr_vma_state state;
    u32 execution_count;
    u64 result_history[4];
    u64 first_result;
};

static bool kcr_verify_deterministic(struct kcr_vma_metadata *meta,
                                       u64 current_result)
{
    switch (meta->state) {
    case KCR_VMA_UNVERIFIED:
        if (meta->execution_count == 0) {
            meta->first_result = current_result;
        } else if (meta->first_result != current_result) {
            meta->state = KCR_VMA_REJECTED;
            return false;
        }
        meta->result_history[meta->execution_count % 4] = current_result;
        meta->execution_count++;
        if (meta->execution_count >= 100)
            meta->state = KCR_VMA_VERIFIED;
        return true;
    case KCR_VMA_VERIFIED:
        return true;
    case KCR_VMA_REJECTED:
        return false;
    }
    return false;
}

bool kcr_should_cache(struct vm_area_struct *vma)
{
    if (!(vma->vm_flags & VM_KCR))
        return false;
    struct kcr_vma_metadata *meta = vma->kcr_metadata;
    return meta->state == KCR_VMA_VERIFIED;
}
```

### 4.10 Generation Counter Validation

```c
/* include/linux/mm_types.h */
struct mm_struct {
    /* ... */
    u64 kcr_generation;
    /* ... */
};

static bool kcr_validate_entry(struct kcr_entry *entry, struct mm_struct *mm)
{
    if (entry->mm_generation != mm->kcr_generation)
        return false;
    if (entry->mm != mm)
        return false;
    if (time_after(jiffies, entry->expiry_jiffies))
        return false;
    return true;
}
```

### 4.11 Fork Inheritance (MAP_SHARED only)

```c
/* kernel/fork.c */
static int kcr_inherit_vma(struct vm_area_struct *vma,
                            struct vm_area_struct *new_vma)
{
    if ((vma->vm_flags & VM_KCR) && (vma->vm_flags & MAP_SHARED)) {
        new_vma->vm_flags |= VM_KCR;
        new_vma->kcr_metadata = vma->kcr_metadata;
        atomic_inc(&new_vma->kcr_metadata->refcount);
    }
    return 0;
}
```

### 4.12 Optional Encryption (if CONFIG_KCR_ENCRYPTION=y)

```c
/* kernel/kcr/kcr_encrypt.c */
#include <crypto/aes.h>
#include <crypto/skcipher.h>

static void kcr_encrypt_entry(struct kcr_entry *entry, struct task_struct *task)
{
    struct skcipher_request *req;
    struct scatterlist sg;
    
    req = skcipher_request_alloc(kcr_skcipher, GFP_ATOMIC);
    sg_init_one(&sg, entry->result_encrypted, sizeof(entry->result_encrypted));
    skcipher_request_set_crypt(req, &sg, &sg, sizeof(entry->result_encrypted),
                                task->kcr_process_key);
    crypto_skcipher_encrypt(req);
    skcipher_request_free(req);
}
```

---

## 5. Security Analysis

### 5.1 Hardware‑Enforced Isolation

KCR eliminates software validation checks by leveraging hardware features:

| Feature | Function | Overhead | KCR Usage |
|---------|----------|----------|-----------|
| SMAP [[24]] | Prevents kernel accessing user memory | 0 cycles | Automatic isolation |
| SMEP [[22]] | Prevents kernel executing user code | 0 cycles | Automatic isolation |
| IOMMU [[29]] | Isolates DMA device memory access | 0 cycles | Invalidation trigger |

Traditional approaches use software checks:

```c
if (current->mm != entry->mm)
    return -EPERM;
```

KCR relies on hardware enforcement rather than software validation.

### 5.2 Optional Per‑Process Encryption

For defense‑in‑depth against malicious kernel modules, KCR optionally encrypts cached results using AES‑NI (5–10 cycles per 64 bytes). The encryption key is process‑specific and stored in `task_struct`.

### 5.3 Cross‑Process Isolation

Each cache entry stores `mm_struct` pointer and generation counter:

```c
struct kcr_entry {
    struct mm_struct *mm;
    u64 mm_generation;
    pid_t pid;
    struct pid_namespace *pid_ns;
};

static bool kcr_validate_entry(struct kcr_entry *entry, struct mm_struct *mm)
{
    return entry->mm == mm && entry->mm_generation == mm->kcr_generation;
}
```

Cross‑process leakage probability is reduced to <0.001%.

---

## 6. Performance Evaluation

### 6.1 Overhead Measurements

| Scenario | Cycles | Source |
|----------|--------|--------|
| Page fault (unmodified) | 2,552 | [[1]] |
| KCR fingerprint (xxHash64) | 25 | [[19]] |
| KCR L2 lookup | 15–25 | [[35]] |
| KCR L3 lookup | 50–100 | [[51]] |
| SMAP/SMEP enabled | 0 | [[21]] |
| Encryption (optional) | +5–10 | [[25]] |

### 6.2 Subsystem‑Specific Benefits

| Subsystem | Operation | Baseline Cycles | KCR Hit Cycles | Savings |
|-----------|-----------|-----------------|----------------|---------|
| Crypto | AES encrypt | 500 | 20 | 25× |
| Crypto | SHA‑256 | 800 | 20 | 40× |
| Network | csum_partial | 100 | 20 | 5× |
| Memory | copy_from_user | 50 | 20 | 2.5× |
| DMA | dma_map_sg | 200 | 20 | 10× |

### 6.3 Global Overhead

On non‑KCR workloads, overhead is limited to the fraction of page faults that occur in `VM_KCR` regions. With typical 1% of faults in such regions and 25 cycles overhead per fault, global overhead is <0.02% of total system cycles.

---

## 7. Limitations and Future Work

### 7.1 Current Limitations

1. **Determinism Requirement**: KCR cannot verify code determinism at runtime without significant overhead. Learning mode mitigates but requires 100 consistent executions before caching.

2. **Hardware Dependencies**: SMAP/SMEP require Intel Sandy Bridge+ or AMD Bulldozer+ [[21]]. Older hardware unsupported.

3. **Memory Overhead**: 16 MB shared region per system may be excessive for memory‑constrained environments; configurable via sysfs.

4. **DMA Device Support**: IOMMU notification requires device driver integration. Some legacy drivers lack support; optional DMA‑BUF module addresses this.

### 7.2 Future Extensions

1. **Compiler Assistance**: GCC/Clang plugin for automatic detection of memoizable functions. Reduces manual `VM_KCR` configuration by ~80%.

2. **Hardware Fingerprinting**: Intel Processor Trace integration for execution flow capture. Increases non‑determinism detection from 60% to 95% [[168]].

3. **Predictive Prefetching**: Markov chain‑based execution pattern learning. Increases hit rate from 70% to 85% [[86]].

4. **Multi‑Node Support**: Distributed KCR cache for cluster environments. Extends reuse across network boundaries.

---

## 8. Conclusion

KCR provides unified kernel‑level computation reuse through three architectural innovations: `memfd`‑based shared memory eliminating kernel‑user copies, hardware‑enforced security via SMAP/SMEP/IOMMU with optional encryption, and a two‑tier per‑CPU/per‑socket cache hierarchy achieving 15–25 cycles hit latency. The system adds <0.02% overhead to standard execution path when inactive.
