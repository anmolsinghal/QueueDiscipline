#pragma once

#include <atomic>
#include <array>
#include <bit>
#include <cstddef>
#include <concepts>
#include <limits>
#include <new>
#include <type_traits>

#include "atomic_entry.hpp"

// Fixed-capacity multiple-producer/multiple-consumer queue.
//
// push() and pop() return false when the ring is full or empty. The capacity
// must be a power of two. T must be trivially copyable and default-initializable.
//
// This specialized queue atomically updates the payload and its
// sequence number as one Entry. For a word-sized T, this requires a lock-free
// double-width CAS. The slot CAS is both the ownership and publication point;
// compilation intentionally fails when the target cannot provide the required
// always-lock-free atomics. GCC and Clang x86-64 builds must enable CMPXCHG16B
// support (for example, with -mcx16). Operations may retry under contention and
// are not wait-free.
template<typename T, std::size_t Size>
requires (std::has_single_bit(Size) && std::is_trivially_copyable_v<T> &&
          std::default_initializable<T>)
class MPMC
{
#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t cacheline_size =
        std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t cacheline_size = 64;
#endif

    static constexpr std::size_t mask = Size - 1;

    struct Entry
    {
        T data;
        std::size_t sequence{0};
    };

    using AtomicEntry = mpmc_detail::atomic_entry<Entry, T>;

    struct alignas(cacheline_size) BufferSlot {
        AtomicEntry entry;
    };

    static_assert(AtomicEntry::is_always_lock_free,
        "MPMC requires a lock-free std::atomic<Entry> or native x86 CMPXCHG16B; "
        "compile supported x86 targets with -mcx16");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
        "MPMC requires lock-free read and write cursors");
    static_assert(Size <= std::numeric_limits<std::size_t>::max() / 2);

    alignas(cacheline_size) std::atomic<std::size_t> write;
    alignas(cacheline_size) std::atomic<std::size_t> read;

    std::array<BufferSlot, Size> slots;

public:
    MPMC() : write{0}, read{0}
    {
        for (std::size_t i = 0; i < Size; ++i)
        {
            Entry empty{T{}, i << 1U};
            slots[i].entry.store(empty, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool push(const T& val)
    {
        while(true)
        {
            std::size_t idx = write.load(std::memory_order_relaxed);
            auto& slot = slots[idx & mask];
            Entry old = slot.entry.load(std::memory_order_acquire);
            const std::size_t seq = old.sequence;

            if(seq == (idx << 1))
            {
                Entry next{val, (idx << 1) | 1U};
                if(slot.entry.compare_exchange_strong(old, next, std::memory_order_acq_rel))
                {
                    write.compare_exchange_strong(idx, idx + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed);
                    return true;
                }
            }
            else if (seq == ((idx << 1) | 1U) || seq == ((idx + Size) << 1))
            {
                write.compare_exchange_strong(idx, idx + 1, std::memory_order_relaxed,
                std::memory_order_relaxed);
            }
            else if((seq + (Size << 1)) == ((idx << 1) | 1U))
            {
                return false;
            }

        }

    }

    [[nodiscard]] bool pop(T& out)
    {
        while(true)
        {
            std::size_t idx = read.load(std::memory_order_relaxed);
            auto& slot = slots[idx & mask];
            Entry old = slot.entry.load(std::memory_order_acquire);
            const std::size_t seq = old.sequence;

            if(seq == ((idx << 1) | 1U))
            {
                Entry next{T{}, (idx + Size) << 1U};
                if(slot.entry.compare_exchange_strong(old, next, std::memory_order_acq_rel))
                {
                    out = old.data;
                    read.compare_exchange_strong(idx, idx + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed);
                    return true;
                }
            }
            else if ((seq | 1U) == (((idx + Size) << 1U) | 1U))
            {
                read.compare_exchange_strong(idx, idx + 1, std::memory_order_relaxed,
                std::memory_order_relaxed);
            }
            else if(seq == (idx << 1))
            {
                return false;
            }

        }
    }

};
