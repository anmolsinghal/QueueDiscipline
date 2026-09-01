#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

// Fixed-capacity single-producer/multiple-consumer queue.
//
// Exactly one thread may call push(); multiple threads may call pop(). Both
// operations return false immediately when the ring is full or empty. The
// capacity must be a power of two. T must be default-constructible and
// move-assignable, and the value passed to push() must be assignable to T.
template<typename T, std::size_t size>
requires (std::has_single_bit(size))
class SPMCQueue
{
#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t cacheline_size =
        std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t cacheline_size = 64;
#endif

    static constexpr std::size_t mask = size - 1; // for fast wraparound

    struct alignas(cacheline_size) Node
    {
        T data;
        std::atomic<std::size_t> index{0};
    };

    alignas(cacheline_size) std::size_t write;
    alignas(cacheline_size) std::atomic<std::size_t> read;

    alignas(cacheline_size) std::array<Node, size> data;

public:
    SPMCQueue() : write{0}, read{0}
    {
        for (std::size_t i = 0; i < size; ++i) {
            data[i].index.store(i, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool pop(T& val) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        std::size_t r = read.load(std::memory_order_relaxed);

        while (true)
        {
            Node& node = data[r & mask];

            if (node.index.load(std::memory_order_acquire) != r + 1)
                return false;

            if (read.compare_exchange_weak(r, r + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
            {
                val = std::move(node.data);
                node.index.store(r + size, std::memory_order_release);
                return true;
            }
        }
    }

    template<typename U>
    requires std::is_assignable_v<T&, U&&>
    [[nodiscard]] bool push(U&& val)
    {
        std::size_t w = write;

        Node& node = data[w & mask];
        std::size_t node_index = node.index.load(std::memory_order_acquire) - w;

        if (node_index == 0)
        {
            node.data = std::forward<U>(val);
            node.index.store(w + 1, std::memory_order_release);
            write = w + 1;
            return true;
        }
        return false;
    }
};
