#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

// Fixed-capacity single-producer/single-consumer queue.
//
// Exactly one thread may call push(), and exactly one other thread may call
// pop(). Both operations return false immediately when the ring is full or
// empty. The capacity must be a power of two. T must be default-constructible,
// copy-assignable for push(), and move-assignable for pop().
template<typename T, std::size_t size>
requires (std::has_single_bit(size))
class SPSCQueue
{
#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t cacheline_size =
        std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t cacheline_size = 64;
#endif

    static constexpr std::size_t mask = size - 1; // for fast wraparound

    alignas(cacheline_size) std::atomic<std::size_t> write{0};
    std::size_t read_cache{0};

    alignas(cacheline_size) std::atomic<std::size_t> read{0};
    std::size_t write_cache{0};

    alignas(cacheline_size) std::array<T, size> data;

public:
    SPSCQueue() = default;

    [[nodiscard]] bool pop(T& val) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        auto r = read.load(std::memory_order_relaxed);

        if (r == write_cache)
        {
            write_cache = write.load(std::memory_order_acquire);
            if (r == write_cache)
                return false;
        }

        val = std::move(data[r & mask]);
        read.store(r + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool push(const T& val) noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        auto w = write.load(std::memory_order_relaxed);

        if (w - read_cache == size)
        {
            read_cache = read.load(std::memory_order_acquire);
            if (w - read_cache == size)
                return false;
        }

        data[w & mask] = val;
        write.store(w + 1, std::memory_order_release);
        return true;
    }
};
