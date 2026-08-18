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

template<typename T, std::size_t Size>
requires PowerOfTwo<Size>
&& std::is_trivially_copyable_v<T> &&
std::default_initializable<T>
class MPMC
{
    struct alignas(hardware_destructive_interference_size) cacheline_atomic {
        std::atomic<std::size_t> value{0};
    };

    static constexpr std::size_t mask = Size - 1;

    struct Entry
    {
        T data;
        std::size_t sequence{0};
    };

    struct alignas(hardware_destructive_interference_size) BufferSlot {
        std::atomic<Entry> entry;
    };

    static_assert(std::atomic<Entry>::is_always_lock_free);
    static_assert(Size <= std::numeric_limits<std::size_t>::max() / 2);

    cacheline_atomic write;
    cacheline_atomic read;

    std::array<BufferSlot, Size> slots;

public:
    MPMC() : write{}, read{}
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
            std::size_t idx = write.value.load(std::memory_order_relaxed);
            auto& slot = slots[idx & mask];
            Entry old = slot.entry.load(std::memory_order_acquire);
            const std::size_t seq = old.sequence;

            if(seq == (idx << 1))
            {
                Entry next{val, (idx << 1) | 1U};
                if(slot.entry.compare_exchange_strong(old, next, std::memory_order_acq_rel))
                {
                    write.value.compare_exchange_strong(idx, idx + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed);
                    return true;
                }
            }
            else if (seq == ((idx << 1) | 1U) || seq == ((idx + Size) << 1))
            {
                write.value.compare_exchange_strong(idx, idx + 1, std::memory_order_relaxed,
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
            std::size_t idx = read.value.load(std::memory_order_relaxed);
            auto& slot = slots[idx & mask];
            Entry old = slot.entry.load(std::memory_order_acquire);
            const std::size_t seq = old.sequence;

            if(seq == ((idx << 1) | 1U))
            {
                Entry next{T{}, (idx + Size) << 1U};
                if(slot.entry.compare_exchange_strong(old, next, std::memory_order_acq_rel))
                {
                    out = old.data;
                    read.value.compare_exchange_strong(idx, idx + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed);
                    return true;
                }
            }
            else if ((seq | 1U) == (((idx + Size) << 1U) | 1U))
            {
                read.value.compare_exchange_strong(idx, idx + 1, std::memory_order_relaxed,
                std::memory_order_relaxed);
            }
            else if(seq == (idx << 1))
            {
                return false;
            }

        }
    }

};
