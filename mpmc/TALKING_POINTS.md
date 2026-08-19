# CppCon Case Study: Double-Width-CAS MPMC Queue

## 1. State the Specialization First

This is a bounded MPMC queue specialized for payloads that can be combined with a sequence number in one always-lock-free atomic object. With the presentation's `uint64_t` payload, `Entry` is 16 bytes: one machine word for the payload and one for the sequence. Enqueue and dequeue therefore depend on a lock-free double-width compare-and-swap.

This is intentionally not a general-purpose queue for arbitrary C++ objects. Compilation requires both `std::atomic<Entry>` and the read/write cursor atomics to be always lock-free on the target.

> Presentation line: “The payload and its ownership state move together in one atomic transaction.”

## 2. Slot State Machine

For logical position `i` and capacity `N`, the sequence encodes:

| Sequence | Meaning |
|---:|---|
| `2 * i` | Free for producer position `i` |
| `2 * i + 1` | Contains the item for consumer position `i` |
| `2 * (i + N)` | Consumed and free for the next lap |

The low bit is the empty/full state. The remaining bits identify the logical generation, preventing an old slot state from being mistaken for the current traversal of the ring.

## 3. Linearization Points

The producer atomically changes:

```text
{old payload, 2*i} -> {new payload, 2*i + 1}
```

That slot CAS is simultaneously the ownership claim and publication point for enqueue.

The consumer atomically changes:

```text
{payload, 2*i + 1} -> {empty payload, 2*(i + N)}
```

That slot CAS captures the value and releases the slot in one transaction. It is the dequeue linearization point.

The global `write` and `read` atomics are cursors, not ownership records. Their CAS operations may lag because another thread can infer completed work from the slot sequence and help advance them.

## 4. Why This Avoids the Earlier Gaps

The MPSC queue separates producer ordering from link publication. The SPMC queue separates consumer ownership from releasing the payload slot. A thread paused between either pair can block the corresponding frontier.

This MPMC design combines each pair in the slot CAS:

* before the CAS, the thread has changed no shared slot state;
* after the CAS, publication or reclamation is already complete;
* if the thread pauses before updating a cursor, another thread helps advance it.

Assuming the required atomics are lock-free, the queue is linearizable and lock-free. It is not wait-free: an individual operation can repeatedly lose CAS attempts under contention.

## 5. Cost of the Stronger Progress Story

For a successful operation, the normal path contains:

1. an atomic load of the entire `Entry`;
2. a double-width CAS on that entry;
3. a best-effort CAS on the global cursor.

Contenders copy and compare the payload as part of the slot CAS. Larger payloads generally make `atomic<Entry>` unavailable or not always lock-free, which rejects the specialization at compile time.

This differs from a conventional sequence-based bounded MPMC queue, which normally CASes a cursor, writes the payload separately, and release-publishes the sequence. The conventional layout supports more payload types and uses a narrower atomic, but reservation and publication are two distinct steps.

## 6. Benchmark Discipline

Do not add a shared per-item completion counter to the timed path. It introduces another atomic RMW and can dominate the queue, particularly at 1P/1C. Consumers should accumulate counts and checksums locally; producers should publish completion once each; validation should happen after joining.

Report items transferred per second rather than ambiguously calling each end-to-end transfer one operation.

## 7. Claims and Limits

* Fixed, power-of-two capacity.
* Trivially copyable, default-initializable payload.
* Whole-entry and cursor atomics must be always lock-free.
* `push()` and `pop()` can retry internally and are not wait-free.
* Slot padding is a target- and workload-dependent tradeoff between false sharing and cache/TLB footprint.
* Destruction requires all producer and consumer threads to have stopped.
