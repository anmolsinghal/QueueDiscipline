#pragma once

#include <atomic>
#include <array>
#include <concepts>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

constexpr bool is_power_of_two(std::size_t n) {
    return n && ((n & (n - 1)) == 0);
}

template<std::size_t N>
concept PowerOfTwo = is_power_of_two(N);

// Specialized bounded MPMC queue that atomically updates the payload and its
// sequence number as one Entry. For a word-sized T, this requires a lock-free
// double-width CAS. The slot CAS is both the ownership and publication point;
// this is intentionally not a general-purpose queue for arbitrary payloads.
template<typename T, std::size_t Size>
requires PowerOfTwo<Size>
&& std::is_trivially_copyable_v<T> &&
std::default_initializable<T>
class MPMC
{
    static constexpr std::size_t mask = Size - 1;

    struct Entry
    {
        T data;
        std::size_t sequence{0};
    };

    struct alignas(hardware_destructive_interference_size) BufferSlot {
        std::atomic<Entry> entry;
    };

    static_assert(std::atomic<Entry>::is_always_lock_free,
        "MPMC requires a lock-free atomic payload-and-sequence Entry");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
        "MPMC requires lock-free read and write cursors");
    static_assert(Size <= std::numeric_limits<std::size_t>::max() / 2);

    alignas(hardware_destructive_interference_size) std::atomic<std::size_t> write;
    alignas(hardware_destructive_interference_size) std::atomic<std::size_t> read;

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
