# SPSC → SPMC: Technical Presentation Reminders

## Start with SPSC

* State the contract first: exactly one producer and exactly one consumer. `push()` and `pop()` are not safe with additional producers or consumers.
* The specialization matters: producer exclusively owns `write`; consumer exclusively owns `read`. No CAS is needed to claim either index.
* It is a bounded, preallocated, power-of-two ring. `index & (capacity - 1)` wraps a monotonically increasing logical index without division.
* Producer path: verify capacity, write the payload, then release-store the new write index.
* Consumer path: acquire-load the published write index when necessary, move the payload out, then release-store the new read index.
* Explain acquire/release in terms of publication: payload writes happen before publishing `write`; observing that publication makes the payload safe to read.
* `relaxed` is used only for a thread's own counter; acquire/release is used when observing or publishing state across threads.
* Each side caches the other side's index and refreshes it only when the ring looks full or empty. The common path avoids a remote atomic load.
* Keep producer and consumer control state on separate interference-size boundaries to avoid false sharing on the indices.
* `push()` / `pop()` return immediately when full / empty; caller-side retry loops are separate from the queue operation itself.
* Do not claim C++ atomics are universally lock-free. Check the target if that claim matters; “bounded, single-attempt operation” is always accurate here.
* Talk about payload size separately from synchronization: large objects turn the result into a data-movement benchmark.

## Then introduce the reason for SPMC

* One producer can still own `write`, but several consumers now need to agree on who receives the next item.
* This is the new shared point of contention: the global `read` index.
* SPMC uses a sequence number in every node because consumers need both answers: “is this slot ready?” and “which consumer owns it?”
* Initial sequence for slot `i` is `i`, meaning it is free for logical producer position `i`.
* Producer checks that a node is free for `w`, writes its payload, then release-stores sequence `w + 1` to publish it.
* Consumer acquire-loads the node sequence and requires `r + 1`, proving the payload is published and visible.
* Several consumers can see the same ready node; a CAS advancing shared `read` from `r` to `r + 1` elects exactly one winner.
* The winning consumer moves the payload, then release-stores `r + capacity` to return the node to the producer for the next lap.
* The per-node sequence acquire/release publishes and protects payload data. The relaxed CAS is only the ownership election.
* The subtraction-based equality checks deliberately work across unsigned logical-index wraparound.

## Explain SPMC performance honestly

* With one consumer, the CAS is uncontended but still extra work that SPSC does not need.
* With several consumers, every successful CAS invalidates the shared `read` cache line for the other consumers.
* More consumers can reduce queue throughput when dequeue is almost all the work. They help when the work after dequeue is independent and substantial enough to amortize contention.
* A roughly balanced item count from one run is not a fairness guarantee.
* Per-node padding avoids interference between neighbouring nodes, but increases memory footprint and can reduce cache residency.

## Bring up the one-consumer prefetch surprise

* The expected algorithmic result: SPSC should beat one-consumer SPMC because SPSC has no CAS on its steady-state path.
* The local result initially reversed: this SPMC beat the current SPSC implementation with one consumer.
* Remove only `__builtin_prefetch` from both queues and rerun the same harness: SPSC becomes roughly 3x faster than SPMC. The prefetch hints—not the CAS—were the dominant difference in that result.
* SPMC prefetches its next 256-byte node; SPSC prefetches its next dense payload slot. A prefetch is a machine-specific performance hint, not a correctness mechanism.
* The 256-byte SPMC node layout can still affect cache traffic, but it was not sufficient to explain the direct one-consumer ranking after the hints were removed.
* `hardware_destructive_interference_size` is an interference-size hint from the library, not a claim about the physical cache-line size.
* The lesson is: benchmark one change at a time. An apparently algorithmic result can be caused by a compiler hint or storage layout.

## Benchmark caveats to mention

* Quote workload, payload size, capacity, compiler optimization, and whether useful consumer work was included.
* Use multiple trials, warm-up, synchronized start, and percentiles or medians rather than one number.
* This M4 has performance and efficiency cores; macOS thread placement makes very short benchmarks variable without strict pinning.
* Separate throughput from latency. Throughput can look excellent while tail latency still reflects interrupts and scheduling.
* Do not generalize local numbers into a universal ranking of SPSC and SPMC.

## Final takeaway

* Choose the narrowest queue contract the application actually satisfies.
* SPSC removes ownership arbitration; SPMC restores it at the shared read index.
* Correct memory ordering is necessary, but performance comes from the whole design: ownership, data layout, cache traffic, payload size, and workload.
