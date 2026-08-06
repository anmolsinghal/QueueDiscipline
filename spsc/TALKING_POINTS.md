# CppCon Presentation Guide: Lock-Free SPSC Queue

This document outlines the core narrative, motivation, real-world use cases, performance targets, and architectural design constraints for presenting a Single Producer Single Consumer (SPSC) Lock-Free Queue at CppCon.

---

## 1. Why We Need SPSC Queues in Life (Motivation)

### The Problem with General-Purpose Synchronization
* **Mutexes & Condition Variables**: Involve OS kernel context switches. A thread context switch costs **1,000 to 5,000+ nanoseconds**, introducing massive latency spikes.
* **Multi-Producer Multi-Consumer (MPMC) Queues**: Rely on atomic `compare_exchange_weak`/`strong` (CAS) loops. Under contention, multiple threads spin-retry the same memory location, triggering **cache-invalidation storms** across CPU cores (interconnect traffic congestion) and unpredictable execution times.

### The SPSC Advantage
* **Asymmetric Thread Invariants**: By enforcing that **only one thread writes to the tail** and **only one thread writes to the head**, we eliminate lock contention entirely.
* **Wait-Free Guarantee**: SPSC queue operations can be implemented to be **wait-free** ($O(1)$ bounded execution time). Neither thread ever loops waiting for another thread to release a lock or update a CAS target.
* **Mechanical Sympathy**: SPSC aligns perfectly with modern CPU hardware architectures (L1/L2 caches, store buffers, and CPU pipeline execution).

---

## 2. Where We Need It (Real-World Use Cases)

SPSC queues are the fundamental building block in latency-critical and real-time software systems:

1. **High-Frequency Trading (HFT) & Financial Exchange Engines**:
   * Decoupling a dedicated Network I/O thread (reading FIX/SBE packets from NIC) from a Trading Strategy thread executing order routing logic.
2. **Real-Time Audio Engines**:
   * Audio callback threads (e.g., CoreAudio, JACK, ASIO) run with real-time OS priority. Taking a mutex or allocating memory (`malloc`) on an audio thread causes **buffer underruns and audible audio crackling**. SPSC ring buffers safely bridge audio processing threads and UI/disk threads.
3. **Game Engine Architecture**:
   * Passing render commands from the main Logic/Physics simulation thread to the dedicated Graphics/Render thread without blocking frame generation.
4. **Asynchronous High-Throughput Telemetry & Logging**:
   * Transporting binary log records out of worker threads to a background Disk I/O or Network flusher thread without stalling the hot application path.
5. **OS Kernel & Device Driver Subsystems**:
   * Inter-process ring buffers (e.g., Linux `io_uring`, VirtIO, eBPF perf ring buffers, and DPDK packet rings) rely on lock-free SPSC ring buffers for zero-copy kernel-user communication.

---

## 3. Performance Requirements

When demoing or building a production-grade SPSC queue, the following performance metrics are critical:

* **Sub-10 Nanosecond Transfer Latency**: Average item transfer latency should be under **5–10 nanoseconds** for scalar types.
* **High Throughput**: Capable of sustaining **100,000,000+ to 250,000,000+ operations/second** on modern CPUs.
* **Deterministic Tail Latency**: Eradicating p99 and p99.9 latency tail spikes by eliminating locks, memory allocation, and unneeded atomic bus locking.
* **Zero Runtime Heap Allocation**: All memory is statically or pre-allocated during queue initialization; zero dynamic allocations occur during `push()` or `pop()`.

---

## 4. Design Constraints & Architectural Principles

### A. Invariants & Capacity Constraints
* **Single Producer / Single Consumer**: Strict precondition. No concurrent `push` calls, no concurrent `pop` calls.
* **Bounded Buffer Capacity**: Fixed capacity specified at compile-time or construction time.
* **Power-of-Two Size & Masking**: Capacity $N$ MUST be a power of two ($N = 2^k$). This allows fast bitwise AND index wrapping:
  $$\text{slot} = \text{index} \& (N - 1)$$
  Replacing expensive hardware integer division (`index % N`, ~10–20 CPU cycles) with a single-cycle bitwise AND (`index & mask`, 1 CPU cycle).

### B. Hardware Interference & Cache Line Isolation
* **False Sharing**: Occurs when data modified by one CPU core resides on the same 64-byte or 128-byte cache line as data modified by another CPU core.
* **Thread-Grouped Layout**:
  * **Producer Cache Line**: Contains `write` atomic index and `read_cache` local index (written ONLY by Producer thread).
  * **Consumer Cache Line**: Contains `read` atomic index and `write_cache` local index (written ONLY by Consumer thread).
* **Dynamic Alignment**: Using C++17 `std::hardware_destructive_interference_size` to ensure portability across 64-byte (x86) and 128-byte (Apple Silicon / ARM / AVX-512) cache line architectures.

### C. Caching Remote Indices (Reducing Cache Coherency Traffic)
* Instead of loading `read.val` (owned by Consumer) on *every single* `push()` call, the Producer maintains a local non-atomic copy `read_cache`.
* The Producer only re-reads `read.val` using an `acquire` atomic load when the queue appears full based on `read_cache`.
* This reduces interconnect bus traffic across CPU sockets/cores by orders of magnitude when the queue is not completely full.

### D. Memory Ordering Constraints
* `std::memory_order_relaxed`: Used for loading local monotonically increasing counters.
* `std::memory_order_release`: Used when storing updated `write` or `read` indices, establishing a memory barrier that ensures data written to the array slot is visible before the index update is published.
* `std::memory_order_acquire`: Used when reading the remote thread's atomic index to synchronize memory visibility.

### E. API Design & Zero-Copy Considerations
* **Pass-by-Reference Output (`bool pop(T& val)`)**: Prevents `std::optional<T>` stack alignment padding and move/copy return overheads.
* **`noexcept` & `[[nodiscard]]`**: Ensures tighter compiler assembly generation and guarantees explicit handling of full/empty states.

---

## 5. Summary Slide Blueprint for CppCon

1. **Slide 1: Why Lock-Free SPSC?** (Mutex context switch cost vs Wait-free SPSC guarantees).
2. **Slide 2: Real-World Use Cases** (HFT, Real-Time Audio, Game Engines, `io_uring`).
3. **Slide 3: Anatomy of False Sharing** (Hardware cache line bouncing diagram).
4. **Slide 4: Memory Order Breakdown** (Relaxed vs Acquire/Release synchronization).
5. **Slide 5: Empirical Benchmarks** (Throughput vs Payload Size & Latency Percentiles p50–p99.9).
