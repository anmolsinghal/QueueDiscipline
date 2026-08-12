# CppCon Case Study: Unbounded Vyukov-Style MPSC Queue

This guide covers the queue in `mpsc/mpsc.hpp`: one consumer receives work from many producers through an unbounded linked list. It is the right contrast to the bounded SPSC and SPMC rings: instead of reserving a fixed array slot, every producer appends a heap node.

---

## 1. Why an Unbounded MPSC Queue Exists

A bounded ring is excellent when a meaningful capacity exists and backpressure is part of the design. It is the wrong default when many independent threads can generate bursty work and a full queue cannot safely reject, block, or spin the producer.

Practical examples:

* **Actor mailbox / event loop:** many threads post work to one owning event-loop thread.
* **Deferred destruction:** arbitrary threads enqueue objects for one thread that owns an allocator, GPU context, or thread-affine resource.
* **Logging and telemetry aggregation:** many application threads hand records to one background writer when occasional bursts matter more than a fixed memory ceiling.
* **Cross-thread notifications:** workers signal one coordinator without requiring every producer to coordinate around a ring's capacity.

“Unbounded” means *not fixed-capacity*. It does **not** mean unlimited: memory allocation can fail, and an overloaded producer population can consume all available memory. The queue replaces an explicit full condition with an allocator and memory-pressure policy.

> Presentation line: “A bounded queue makes overload visible as `full`. An unbounded queue moves that overload decision to memory management.”

---

## 2. Contract and Shape

The implementation has:

* any number of producer threads calling `push()`;
* exactly one consumer thread calling `pop()`;
* a queue-owned dummy node at the consumer end;
* an atomic producer endpoint, `tail`;
* one newly allocated node per successful `push()`.

`head` is consumer-owned. It always points at the current dummy node; the first real item is `head->next`. `tail` is the producer endpoint. The names are less important than the ownership: only the consumer advances `head`; all producers atomically replace `tail`.

The queue is non-intrusive: `Node` stores `T` directly and the queue allocates/deletes it. The classic high-performance form is often intrusive, where the caller embeds a queue node in a larger object and controls allocation/reclamation externally.

---

## 3. Producer Protocol: Exchange Then Link

```cpp
Node* node = new Node{value, nullptr};
Node* previous = tail.exchange(node, std::memory_order_acq_rel);
previous->next.store(node, std::memory_order_release);
```

The atomic exchange serializes producers in one read-modify-write operation. Every producer receives a unique predecessor; no CAS retry loop is needed.

Two producers can overlap safely:

```text
P1: previous = exchange(A)  // predecessor is stub
P2: previous = exchange(B)  // predecessor is A
P2: A->next = B
P1: stub->next = A

reachable list: stub → A → B
```

The release store to `previous->next` publishes the initialized node and its payload to the consumer.

> Presentation line: “Exchange chooses my predecessor. Linking through that predecessor makes me visible.”

---

## 4. Consumer Protocol: Advance the Dummy

```cpp
Node* next = head->next.load(std::memory_order_acquire);
if (next == nullptr)
    return false;

value = std::move(next->data);
Node* old_head = head;
head = next;
delete old_head;
```

The acquire load that sees `next` synchronizes with the producer's release store, so `next->data` is ready to read.

The consumer does not delete `next`. It promotes `next` into the new dummy node and deletes the *previous* dummy. This is safe because observing `old_head->next == next` proves that the producer has already finished the only link write it makes through `old_head`.

This moving-dummy technique gives the single consumer simple reclamation without hazard pointers: producers only write `previous->next`, and the consumer deletes a predecessor only after it has observed that link.

---

## 5. The Important Gap: Published Endpoint, Unlinked List

The producer exchange and link are two separate operations:

```text
P1: tail.exchange(A)        // A is now the producer endpoint
    ... P1 is paused here ...
P1: previous->next.store(A) // A becomes reachable from the consumer
```

While P1 is paused, another producer can append B after A, but neither A nor B is reachable from the consumer until P1 completes the missing link.

Consequences to state precisely:

* `pop()` returning `false` can mean either **truly empty** or **a producer is between exchange and link**.
* If the paused producer never resumes, the consumer cannot reach that suffix. The queue as a whole is blocking in this failure mode.
* The algorithm is serializable, not linearizable: it preserves FIFO order within each producer, but does not provide a single global linearization point for overlapping producers.

This is the defining tradeoff of the minimal Vyukov MPSC design. It is often acceptable when producer threads cannot be cancelled in the exchange-to-link window and a very small producer path matters more than strict global queue semantics.

> Presentation line: “We traded a CAS loop for a two-step handoff. The tiny gap between those steps is the whole algorithm’s caveat.”

---

## 6. Performance and Operational Tradeoffs

| Strength | Cost |
|---|---|
| No fixed capacity or full-ring check | Heap allocation and eventual deallocation per item in this non-intrusive implementation |
| One atomic exchange per producer protocol | A stalled producer can make a suffix unreachable |
| No producer CAS retry loop | The complete `push()` is not wait-free when `new` is included |
| Single consumer has simple dummy-node reclamation | Only one consumer may call `pop()` |
| Per-producer FIFO | No global linearizable FIFO order among concurrent producers |

The local comparison benchmark intentionally makes these costs visible. With a tiny `uint64_t` payload and no useful work, the unbounded MPSC queue was roughly 10–20 M ops/s, much slower than the preallocated rings. That is expected: allocation, pointer chasing, allocator metadata, and cache/TLB misses dominate a scalar queue benchmark.

Use this queue when its unbounded, many-producer message-passing semantics solve a real application problem. Do not choose it for a real-time producer, an allocation-free hot path, or a workload where a known bounded capacity and backpressure policy are available.

---

## 7. Implementation Limits to Mention

* `T` must currently be default-constructible because the dummy node contains a `T`.
* `push(const T&)` copies only; move-only payloads are not supported by this version.
* A throwing move assignment in `pop()` can interrupt item delivery after the node link has been observed; prefer non-throwing payload moves for a presentation-quality queue.
* Destruction requires all producers and the consumer to have stopped first.
* `new` can throw and can take locks internally. The queue protocol is simple, but this non-intrusive wrapper is not suitable for hard real-time code.
* `pop()` has only a boolean result, so callers cannot distinguish a true empty queue from the exchange-to-link gap. A richer `empty/item/retry` API can expose that distinction.

---

## 8. Connection to the Other Queues

* **SPSC:** ownership is predetermined at both ends; bounded array; no CAS; best choice when one producer and one consumer are enough.
* **SPMC:** the producer remains unique, but consumers contend to claim `read`; bounded array with per-slot sequence numbers.
* **MPSC:** consumer remains unique, but producers serialize by exchanging the producer endpoint; linked nodes avoid a fixed capacity at the price of allocation and the exchange-to-link gap.

The broader lesson is not that one queue is best. Concurrency contracts, overload policy, allocation policy, and progress guarantees are application-level decisions that determine the right data structure.

## References

* Dmitry Vyukov’s intrusive MPSC algorithm, as implemented and documented by [grivet/mpsc-queue](https://github.com/grivet/mpsc-queue).
* [Ode to a Vyukov Queue](https://int08h.com/post/ode-to-a-vyukov-queue/) for a clear explanation of the blocking corner case and serializable ordering.
