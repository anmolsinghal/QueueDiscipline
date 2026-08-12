# CppCon Case Study: SPMC Queue and the Prefetch Surprise

This is a focused guide for presenting the `SPMCQueue` implementation in this directory. The useful story is not merely “more consumers mean more throughput.” The queue shows the opposite: a single producer can feed many consumers safely, but every consumer contends for one dequeue position. It also produced a surprising local result: with one consumer, this SPMC implementation briefly outperformed the current SPSC implementation. A controlled follow-up showed the dominant cause was the compiler prefetch hints, not a magical advantage for CAS.

---

## 1. The Contract and the Trade

`SPMCQueue<T, size>` is a fixed-capacity, power-of-two ring buffer with:

* exactly one producer calling `push()`;
* one or more consumers calling `pop()` concurrently;
* no allocation after construction;
* a single-attempt API: `push()` or `pop()` returns `false` instead of internally waiting or retrying.

The producer owns `write`, so enqueue does not need a CAS. Consumers share `read`, so dequeue must atomically decide which consumer owns the next item. That is the fundamental SPMC cost.

The `size` template parameter must be a power of two. `index & (size - 1)` maps a monotonically increasing logical position back into the ring without division.

> Presentation line: SPSC removes ownership arbitration because ownership is predetermined. SPMC restores arbitration at exactly one point: “which consumer gets the next item?”

---

## 2. Per-Node Sequence Numbers

Each `Node` contains the payload and an atomic `index` sequence number. At construction, slot `i` receives sequence `i`.

For logical positions `w` (producer) and `r` (consumer), a slot follows this lifecycle:

| State | Sequence value | Meaning |
|---|---:|---|
| Free initially | `i` | Slot `i` may accept producer position `i`. |
| Free after a full lap | `r + size` | The consumer has returned this slot for the producer's next cycle. |
| Published | `w + 1` | The producer has written the payload at logical position `w`. |
| Claimed | `read` advanced from `r` to `r + 1` | Exactly one consumer owns the published node. |

The producer checks `node.index == w`, writes `node.data`, then release-stores `w + 1`. A consumer acquire-loads the sequence and checks for `r + 1`. Several consumers can observe a ready node, but only one can advance the shared `read` counter from `r` to `r + 1` with CAS. The winner moves the payload and release-stores `r + size`, making the slot reusable.

The implementation expresses the equality checks as unsigned subtraction, for example:

```cpp
node.index.load(std::memory_order_acquire) - (r + 1) == 0
```

That preserves the equality test across unsigned counter wraparound.

---

## 3. Memory Ordering: Two Separate Jobs

| Operation | Ordering | Job |
|---|---|---|
| Producer publishes `w + 1` to the node | Release store | Makes the payload write visible before the node is announced. |
| Consumer reads the node sequence | Acquire load | Observes the published payload before moving it. |
| Consumer claims global `read` | Relaxed CAS | Chooses exactly one consumer; visibility already came from the node-sequence acquire. |
| Consumer releases `r + size` to the node | Release store | Finishes reading/moving the payload before the producer can reuse the slot. |
| Producer checks whether the node is reusable | Acquire load | Observes the consumer's completed release before overwriting the payload. |

The relaxed CAS is deliberate: it is an ownership election, not the payload publication mechanism. The acquire of the per-node sequence is what synchronizes the producer's payload write with the consumer's read.

> Presentation line: The global index answers “who gets it?” The per-node sequence answers “is the data ready and safe to touch?”

---

## 4. Why More Consumers Reduce Throughput

The single `read` counter is a hot shared cache location. With one consumer, every CAS succeeds without inter-consumer contention. With two or more consumers, each successful dequeue invalidates the cache copy held by the others, and losing consumers return `false` before retrying in their caller's loop.

This makes SPMC a good fit when work performed after dequeue is substantial or naturally parallel. It is a poor fit when consumers do almost no work and the queue itself becomes the shared bottleneck.

An optimized local run of the included benchmark with 10 million `uint64_t` items and capacity 1024 reported:

| Consumers | Throughput | Average | Work distribution |
|---:|---:|---:|---|
| 1 | 382.10 M ops/s | 2.62 ns/op | 100% |
| 2 | 21.32 M ops/s | 46.90 ns/op | 50.3% / 49.7% |
| 4 | 7.64 M ops/s | 130.86 ns/op | Roughly even |
| 8 | 4.04 M ops/s | 247.39 ns/op | Roughly even |

These numbers are a demonstration of contention, not portable performance claims. The benchmark does not pin threads or isolate CPU topology, and it measures an extremely small payload with no useful consumer work. The queue does not promise fairness; a roughly even distribution in one run is not a fairness guarantee.

---

## 5. The One-Consumer Prefetch Surprise

### What we expected

With one producer and one consumer, SPSC should normally be the simpler and faster design. Its steady-state path has no CAS. The SPMC consumer still performs a successful CAS per item, plus a per-node sequence check.

On Apple Silicon, optimized assembly reflects that extra work:

* SPSC's normal path loads its local counter, accesses payload storage, and release-publishes its own counter.
* SPMC's consumer additionally acquire-loads the node sequence and executes an atomic `cas` on the shared `read` position.

The result was nevertheless reversed in initial measurements on the local M4 MacBook Air.

### Isolating the difference

Both queues originally issued `__builtin_prefetch` for their next slot on every successful operation. A matched comparison with prefetch enabled showed SPMC narrowly ahead on this M4. Removing only those hints reversed the result: in a 2-million-item, five-trial run, median SPSC throughput was 178.93 M ops/s while one-consumer SPMC was 58.37 M ops/s.

This is the expected algorithmic ordering: SPSC has no CAS on its normal dequeue path. The prefetch instruction is a performance hint, not a semantic guarantee, and its impact differs sharply for the queues' layouts.

### A secondary layout observation

On this Apple Clang/libc++ toolchain:

```cpp
std::hardware_destructive_interference_size == 256
```

`SPMCQueue` applies that alignment to every `Node`. For `uint64_t`, an 8-byte payload therefore occupies a 256-byte node. The current SPSC queue stores `uint64_t` payloads at 8-byte stride.

The layouts are still materially different:

```text
Current SPSC payload storage
[ item 0 ][ item 1 ][ item 2 ][ item 3 ] ...
     8 B       8 B       8 B

Current SPMC node storage on this toolchain
[ data 0 + sequence 0 ][ padding ... ][ data 1 + sequence 1 ]
          256 B                          256 B
```

Producer and consumer must always hand the *same item* from one cache hierarchy to the other. Padding cannot remove that necessary transfer. It can, however, prevent producer activity on one item from disturbing consumer activity on a different, neighbouring item. Dense SPSC storage makes that false sharing possible; the widely spaced SPMC nodes largely avoid it.

The SPMC hint prefetches a subsequent 256-byte node on this toolchain; SPSC's hint prefetches the next dense payload position. That different access pattern can make a prefetch highly beneficial for one implementation and less so for another.

### What the experiments showed

Barrier-synchronized, checksum-validated local trials used one producer, one consumer, capacity 1024, 10 million `uint64_t` items, `-O3 -DNDEBUG`, and a warm-up. They showed:

| Layout / queue | Typical local observation |
|---|---|
| SPSC, prefetch enabled | 258.67 M ops/s median in one matched 2M-item run. |
| SPMC, one consumer, prefetch enabled | 266.32 M ops/s median in the same run. |
| SPSC, prefetch removed | 178.93 M ops/s median. |
| SPMC, one consumer, prefetch removed | 58.37 M ops/s median. |

The key teaching point is experimental discipline: change one variable at a time. The earlier storage-layout observations may still matter, but they did not explain the direct result until the prefetch hints were isolated.

### What this does **not** prove

* `hardware_destructive_interference_size` is a conservative library interference-size hint, not a measurement of the physical cache-line size.
* The initial ranking does not demonstrate that SPMC's CAS is cheaper than SPSC's no-CAS path; it disappeared when the prefetch hints were removed.
* The M4 has performance and efficiency cores. macOS did not provide strict core pinning in these runs, so placement can move results substantially.
* SPMC did not become algorithmically cheaper than SPSC. Cache behavior outweighed instruction-count differences for this specific implementation and run.

> Presentation line: “The algorithm told us SPSC should win. The benchmark disagreed. Removing one performance hint showed us what we had actually measured.”

---

## 6. Terminology and Limits

This code has bounded, single-attempt queue operations: neither `push()` nor `pop()` contains a retry loop. Calling code may still spin by repeatedly calling an operation after it returns `false`.

Avoid an unconditional language-level “lock-free” claim. Whether `std::atomic<size_t>` is lock-free is target-dependent. On the intended desktop targets it is commonly lock-free, but the C++ type itself does not guarantee it. Likewise, a throwing move assignment in `pop()` occurs after the node has been claimed; for presentation and production use, prefer payload types with non-throwing move assignment.

---

## 7. Suggested Slide and Demo Sequence

1. **Why SPMC?** One producer can distribute independent work to many consumers.
2. **The ownership problem.** Highlight the one shared `read` CAS.
3. **Node sequence lifecycle.** Draw `free → published → claimed → free` with the sequence values.
4. **Memory-ordering split.** Node sequence publishes data; shared CAS elects a consumer.
5. **Scaling experiment.** Show contention growing from one to eight consumers.
6. **The surprise.** SPMC with one consumer initially beats SPSC locally.
7. **The diagnosis.** Remove prefetch from both queues; SPSC regains its expected advantage.
8. **The lesson.** Measure one variable at a time: hints, layout, cache traffic, topology, and scheduling all matter.
