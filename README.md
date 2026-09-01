# Queue Discipline

Companion code for a CppCon talk about designing concurrent queues around the
narrowest producer/consumer contract an application can guarantee.

The repository contains four focused C++23 queue implementations. They are
small enough to study directly and intentionally expose the tradeoffs among
ownership, contention, capacity, allocation, and progress guarantees.

> These are educational implementations, not a drop-in production queue
> library. Read the contracts and limitations before using them in an
> application.

## Choose the topology first

| Queue | Producers | Consumers | Storage | When full or empty |
| --- | ---: | ---: | --- | --- |
| [`SPSCQueue<T, N>`](spsc/spsc.hpp) | One | One | Bounded ring | `push`/`pop` return `false` |
| [`SPMCQueue<T, N>`](spmc/spmc.hpp) | One | Many | Bounded ring | `push`/`pop` return `false` |
| [`mpsc<T>`](mpsc/mpsc.hpp) | Many | One | Unbounded linked list | `pop` spins until an item arrives |
| [`MPMC<T, N>`](mpmc/mpmc.hpp) | Many | Many | Bounded ring | `push`/`pop` return `false` |

The topology is part of the type's safety contract. Calling `push` from too
many producer threads or `pop` from too many consumer threads is not a
performance mistake—it is incorrect use of the queue.

For every bounded queue, `N` is a compile-time capacity and must be a power of
two.

## The talk's progression

### 1. SPSC: ownership is predetermined

With exactly one producer and one consumer, each side owns its position:

- the producer writes `write`;
- the consumer writes `read`;
- neither side needs compare-and-swap to claim an item;
- each side caches the other position to reduce shared atomic loads.

The producer writes the payload and then release-stores the new write position.
The consumer acquire-loads that position before reading the payload. The read
position uses the same acquire/release pattern in the opposite direction when
a slot becomes reusable.

There is no retry loop inside `push` or `pop`; a full or empty ring is reported
immediately.

### 2. SPMC: consumers must elect an owner

The producer still owns the write position, but consumers now share the read
position. Each slot has a sequence number that answers whether the slot belongs
to the current logical turn.

Several consumers may observe the same ready slot. A compare-and-swap on the
shared read cursor elects exactly one winner. The slot sequence publishes the
payload and later makes the slot reusable; the cursor CAS only decides who owns
the dequeue.

`pop` can retry while other consumers win that election. The shared read cursor
is also the principal contention point, so more consumers do not automatically
mean more queue throughput.

### 3. MPSC: producers serialize at the tail

The MPSC implementation uses an unbounded linked list with a moving dummy node.
Every producer:

1. allocates and initializes a node;
2. exchanges the shared tail to obtain a unique predecessor;
3. release-stores the link from that predecessor.

The single consumer follows the published link, moves out the payload, and
deletes the previous dummy node. This avoids a general-purpose reclamation
scheme because only one consumer advances the head.

`pop` is intentionally a busy-waiting operation. It spins when the queue is
empty, and it can also wait when a producer is paused between exchanging the
tail and publishing its predecessor link. Each `push` allocates, so this version
is not suitable for a hard real-time path.

### 4. MPMC: payload and slot state move together

The MPMC queue combines a payload and sequence number in one atomic entry. Its
slot compare-and-swap is both the ownership and publication point. Global read
and write positions are cursors that other threads may help advance.

This is a deliberately specialized design. The combined entry must be
always-lock-free on the target. For a word-sized payload on GCC or Clang
x86-64, enable `CMPXCHG16B`, for example with `-mcx16`. The internal
[`atomic_entry`](mpmc/atomic_entry.hpp) adapter uses either an always-lock-free
standard atomic or the native x86 double-width primitive.

MPMC operations can retry under contention. When the required atomics are
lock-free, the algorithm is lock-free but not wait-free. Compilation fails with
a diagnostic when the target cannot meet the atomic requirements.

## Basic usage

All implementations are header-only. Add the repository root to the include
path and include the queue you need.

### Bounded queues

```cpp
#include "spsc/spsc.hpp"

#include <cstdint>

SPSCQueue<std::uint64_t, 1024> queue;

void transfer_one()
{
    std::uint64_t produced = 42;
    while (!queue.push(produced)) {
        // Apply the producer's retry or backoff policy.
    }

    std::uint64_t consumed = 0;
    while (!queue.pop(consumed)) {
        // Apply the consumer's retry or backoff policy.
    }
}
```

The SPMC and MPMC APIs have the same full/empty shape:

```cpp
#include "spmc/spmc.hpp"
#include "mpmc/mpmc.hpp"

#include <cstdint>

SPMCQueue<std::uint64_t, 1024> work_distribution;
MPMC<std::uint64_t, 1024> shared_queue;
```

Only use the MPMC declaration on a target that satisfies its compile-time
atomic checks.

### MPSC

```cpp
#include "mpsc/mpsc.hpp"

mpsc<int> inbox;

void receive_one()
{
    inbox.push(42); // May be called by multiple producer threads.

    int value = 0;
    inbox.pop(value); // Exactly one consumer; waits until an item is reachable.
}
```

## Payload requirements

| Queue | Requirements on `T` |
| --- | --- |
| SPSC | Default-constructible, copy-assignable for `push`, move-assignable for `pop` |
| SPMC | Default-constructible and move-assignable; the argument to `push` must be assignable to `T` |
| MPSC | Default-constructible, copy-constructible, and move-assignable |
| MPMC | Trivially copyable and default-initializable; payload and sequence must form an always-lock-free atomic entry |

Prefer non-throwing payload operations. These compact examples do not provide a
production recovery policy for an exception after an item or slot has been
claimed.

## Build requirements

- C++23 compiler
- Threads enabled for applications that use the queues concurrently
- A target with appropriate lock-free atomics for the selected implementation

Example compile command:

```sh
c++ -std=c++23 -O2 -pthread -I/path/to/QueueDiscipline example.cpp
```

For the native x86-64 MPMC backend with GCC or Clang:

```sh
c++ -std=c++23 -O2 -pthread -mcx16 \
    -I/path/to/QueueDiscipline example.cpp
```

## Important limitations

- Stop every producer and consumer before destroying its queue.
- The implementations do not provide built-in sleeping, notification, or
  backoff policies.
- MPSC is "unbounded" only in the sense that it has no fixed capacity; memory
  allocation can still fail, and producers can outpace available memory.
- `std::hardware_destructive_interference_size` is used as a layout hint when
  available. It is not a universal statement about the machine's physical
  cache-line size.
- Lock-free does not mean wait-free, fair, or faster for every workload.
- Benchmark results depend on payload size, topology, CPU, compiler, thread
  placement, contention, and the useful work performed around the queue.

## Repository layout

```text
spsc/spsc.hpp          Single-producer/single-consumer bounded ring
spmc/spmc.hpp          Single-producer/multiple-consumer bounded ring
mpsc/mpsc.hpp          Multiple-producer/single-consumer linked queue
mpmc/mpmc.hpp          Multiple-producer/multiple-consumer bounded ring
mpmc/atomic_entry.hpp  MPMC atomic-entry portability adapter
```

## License

Licensed under the [Apache License 2.0](LICENSE).
