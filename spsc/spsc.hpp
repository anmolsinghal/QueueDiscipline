#pragma once

#include <atomic>
#include <array>
#include <concepts>
#include <cstddef>
#include <utility>
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

template<typename T, std::size_t size>
requires PowerOfTwo<size>
class SPSCQueue
{   
    struct alignas(hardware_destructive_interference_size) cacheline_atomic {
        std::atomic<std::size_t> val{0};
    };
    static constexpr std::size_t mask = size - 1; // for fast wraparound

    // --- Producer-owned Cache Line (only Producer thread writes here) ---
    cacheline_atomic write;
    std::size_t read_cache{0};

    // --- Consumer-owned Cache Line (only Consumer thread writes here) ---
    cacheline_atomic read;
    std::size_t write_cache{0};

    // --- Storage Line ---
    alignas(hardware_destructive_interference_size) std::array<T, size> data;

public:
    SPSCQueue() : write{}, read{} {}

    // Pass-by-reference pop
    [[nodiscard]] bool pop(T& val) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        auto r = read.val.load(std::memory_order_relaxed);

        if (r == write_cache)
        {
            write_cache = write.val.load(std::memory_order_acquire);
            if (r == write_cache)
                return false;
        }

        val = std::move(data[r & mask]);
        read.val.store(r + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool push(const T& val) noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        auto w = write.val.load(std::memory_order_relaxed);

        if (w - read_cache == size)
        {
            read_cache = read.val.load(std::memory_order_acquire);
            if (w - read_cache == size)
                return false;
        }

        data[w & mask] = val;
        write.val.store(w + 1, std::memory_order_release);
        return true;
    }
};