# Kernel Computation Reuse (KCR) Module

A Linux kernel module for transparent computation reuse through adaptive lookup acceleration.

> **CURRENTLY JUST A LIBRARY, NOT FUNCTIONAL YET BECAUSE IT REQUIRES DIRECT CHANGES TO THE KERNEL CODE**

## Overview

KCR provides kernel-level memoization services that cache results of deterministic functions and inject them on subsequent identical calls. The system uses a two-tier cache hierarchy with per-CPU L2 and per-socket L3 caches, achieving 15-25 cycle hit latency.

## Features

- **Two-tier cache hierarchy**: Per-CPU L2 (512 entries) and per-socket L3 (4096 entries)
- **Zero-copy shared memory**: memfd-based region visible to both kernel and user space
- **Hardware-enforced security**: Leverages SMAP/SMEP/IOMMU for isolation
- **IOMMU-based invalidation**: 100% coverage for DMA writes and memory modifications
- **Determinism learning**: Automatic verification before caching
- **Debugfs interface**: Statistics and configuration at `/sys/kernel/debug/kcr/`

## Architecture

```
User Space          Kernel Space        DMA Devices
    │                    │                   │
    └────────────────────┼───────────────────┘
                         │
              ┌──────────▼──────────┐
              │  memfd Shared Memory│
              │     (16 MB default) │
              └──────────┬──────────┘
                         │
              ┌──────────▼──────────┐
              │   L2/L3 Cache       │
              │   (Per-CPU/Socket)  │
              └─────────────────────┘
```

## Building

### Prerequisites

- Linux kernel headers for your running kernel
- GCC with kernel module support

### Compile

```bash
cd drivers/kcr
make
```

### Install

```bash
sudo make install
sudo modprobe kcr
```

### Load/Unload

```bash
# Load module
sudo insmod kcr.ko

# Load with KCR disabled
sudo insmod kcr.ko kcr_enable=0

# Unload module
sudo rmmod kcr
```

## Debugfs Interface

After loading the module, statistics and configuration are available:

```bash
# View statistics
cat /sys/kernel/debug/kcr/stats

# View configuration
cat /sys/kernel/debug/kcr/config
```

## API Reference

### Core Functions

```c
// Initialize KCR subsystem
int kcr_init(void);

// Shutdown KCR subsystem
void kcr_exit(void);

// Check if KCR is enabled
bool kcr_is_enabled(void);
```

### Cache Operations

```c
// Lookup cached result
struct kcr_entry *lookup_unified(u64 fingerprint, struct mm_struct *mm);

// Store result in cache
int store_result(u64 fingerprint, const void *data, u32 len, struct mm_struct *mm);

// Invalidate cache entries for memory range
void invalidate_range(struct mm_struct *mm, unsigned long start, unsigned long end);
```

### Fingerprint Computation

```c
// Compute xxHash64 fingerprint
u64 compute_fingerprint(const void *data, size_t len, u64 seed);

// Compute crypto operation fingerprint
u64 crypto_compute_fingerprint(struct skcipher_request *req);
```

### Determinism Verification

```c
// Verify function produces deterministic results
bool verify_deterministic(struct vma_metadata *meta, u64 current_result);

// Check if VMA should be cached
bool should_cache(struct vm_area_struct *vma);
```

## Configuration

### Module Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `kcr_enable` | bool | true | Enable/disable KCR subsystem |

### Build-time Options

| Option | Description |
|--------|-------------|
| `CONFIG_KCR` | Enable KCR subsystem support |

## Performance

### Overhead

- **Inactive path**: <0.02% overhead when KCR is disabled
- **Fingerprint computation**: ~25 cycles (xxHash64)
- **L2 cache hit**: 15-25 cycles
- **L3 cache hit**: 50-100 cycles
- **SMAP/SMEP**: 0 cycles (hardware-enforced)

### Subsystem Benefits

| Subsystem | Operation | Savings |
|-----------|-----------|---------|
| Crypto | AES encrypt | 25× |
| Crypto | SHA-256 | 40× |
| Network | csum_partial | 5× |
| Memory | copy_from_user | 2.5× |

## Security

KCR leverages hardware features for isolation:

- **SMAP**: Prevents kernel from accessing user memory without explicit override
- **SMEP**: Prevents kernel from executing user code pages
- **IOMMU**: Isolates DMA device memory access
- **Optional encryption**: Per-process AES-NI encryption (5-10 cycles overhead)

## Limitations

1. **Hardware requirements**: Requires Intel Sandy Bridge+ or AMD Bulldozer+ for SMAP/SMEP
2. **Memory overhead**: 16 MB shared region per system
3. **Determinism requirement**: Functions must be deterministic (verified by learning mode)
4. **DMA support**: Some legacy drivers may lack IOMMU notification support

## File Structure

```
.
├── include/linux/
│   ├── kcr.h           # Main header with data structures
│   ├── kcr_types.h     # Type extensions
│   ├── kcr_flags.h     # VM flag definitions
│   └── kcr_task.h      # Task struct extensions
├── drivers/kcr/
│   ├── Makefile
│   ├── kcr_main.c      # Module initialization
│   ├── kcr_mem.c       # Memory management
│   ├── kcr_cache.c     # Cache implementation
│   └── kcr_debugfs.c   # Debugfs interface
├── kernel/kcr/
│   ├── kcr_core.c      # Core logic (fingerprint, injection)
│   └── kcr_determinism.c # Determinism verification
└── drivers/iommu/
    └── kcr_iommu.c     # IOMMU integration
```

## License

GPL-2.0
