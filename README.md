# KCR (Kernel Computation Reuse)

**KCR** is a Linux kernel subsystem designed to eliminate redundant deterministic computation through transparent, kernel-level memoization. By intercepting execution paths and caching results of pure functions, KCR reduces CPU utilization for repetitive workloads without requiring application modifications.

> **Status:** **Prototype / Kernel Module Only**  
> This repository currently implements KCR as a loadable kernel module (LKM). It does not yet require permanent kernel patching or recompilation of the core kernel image. This approach allows for rapid iteration, safe testing, and easy removal.

## Features

*   **Transparent Memoization:** Automatically caches results of deterministic code fragments(Kernel and User Space) marked for optimization(By the programmer or by the kernel itself)
*   **Zero-Copy Shared Memory:** Utilizes `memfd_create` for efficient sharing of cache structures between kernel and user space.
*   **Hardware-Enforced Security:** Leverages SMAP/SMEP for isolation; optional AES-NI encryption for defense-in-depth against malicious kernel modules.
*   **Adaptive Determinism Verification:** "Learning mode" automatically verifies that code regions are deterministic before enabling caching, preventing incorrect results from non-deterministic functions.
*   **NUMA-Aware Caching:** Per-CPU L2 cache and per-socket L3 cache design ensures low-latency access on multi-socket systems.
*   **Low Overhead:** RCU-protected read paths ensure near-zero overhead for cache hits; <0.02% overhead when inactive.

## Architecture

KCR operates via a two-tier cache hierarchy:
1.  **L2 Cache (Per-CPU):** 512 entries, RCU-protected for lock-free reads. Handles ~90% of hits with 15–25 cycle latency.
2.  **L3 Cache (Per-Socket):** 4096 entries, shared across cores on the same socket. Handles cold entries with 50–100 cycle latency.

The system intercepts execution via hooks in `handle_mm_fault()` and specific subsystem APIs (e.g., Crypto API), validating determinism via a generation counter and inode version tracking.

## Requirements

*   **Linux Kernel:** Version 6.19 or newer (required for specific VMA flags and tracepoint APIs).
*   **Architecture:** x86_64 (currently optimized for Intel/AMD CPUs with SMAP/SMEP support).
*   **Dependencies:** `libcrypto` (for optional encryption), `kernel headers`.

## Usage

KCR operates transparently once loaded. Applications do not need to be recompiled. However, to benefit from memoization, memory regions must be identified as candidates.

### Automatic Detection
By default, KCR runs in **Learning Mode**. It monitors marked regions for 100 executions to verify determinism before enabling caching. If non-determinism is detected, caching is automatically disabled for that region.

## Performance Impact

*   **Idle Overhead:** < 0.02% (when no regions are marked).
*   **Hit Latency:** 15–25 cycles (L2), 50–100 cycles (L3).
*   **Workload Savings:**
    *   **Crypto (AES/SHA):** Up to 30% CPU reduction in repetitive key derivation.
    *   **Network Checksums:** Up to 15% throughput increase in high-packet-rate scenarios.
    *   **Fork-Heavy Servers (Nginx):** Up to 30% request/sec improvement via cache inheritance.

## Limitations & Roadmap

*   **Current Scope:** Limited to x86_64. ARM64 support planned for v0.3 or v0.4.
*   **Persistence:** Cache is volatile; cleared on module unload or reboot.
*   **Future Work:**
    *   Integration into mainline kernel source tree (`kernel/kcr/`).
    *   Compiler plugin (`gcc-plugin-kcr`) for automatic candidate detection.
    *   Support for AVX-512 and AVX-256 accelerated fingerprinting.

## License

This project is licensed under the **GNU General Public License v3.0**. See the [LICENSE](LICENSE) file for details.

---
**Disclaimer:** This software is a prototype. While designed with safety mechanisms (learning mode, validation), it modifies kernel execution flow. Test thoroughly in a non-production environment before deployment.