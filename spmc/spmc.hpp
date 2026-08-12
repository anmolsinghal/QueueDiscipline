#include <atomic>
#include <array>
#include <concepts>
#include <cstddef>
#include <utility>
#include <new>
#include <type_traits>
#include <memory>
#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
#endif

#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

constexpr bool is_power_of_two(std::size_t n) {
    return n && ((n & (n - 1)) == 0);
};

template<std::size_t N>
concept PowerOfTwo = is_power_of_two(N);

template<typename T, std::size_t size>
requires PowerOfTwo<size>
class SPMCQueue
{   
    struct alignas(hardware_destructive_interference_size) cacheline_atomic {
        std::atomic<std::size_t> val{0};
    };

    static constexpr std::size_t mask = size - 1; // for fast wraparound

    struct alignas(hardware_destructive_interference_size) Node
    {
        T data;
        std::atomic<size_t> index{0};
    };

    size_t write;
    cacheline_atomic read;

    alignas(hardware_destructive_interference_size) std::array<Node, size> data;

public:
    SPMCQueue() : write{0}, read{} 
    {
        for (size_t i = 0; i < size; ++i) {
            data[i].index.store(i, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool pop(T& val) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        size_t r = read.val.load(std::memory_order_relaxed);

        Node& node = data[r & mask];
        std::size_t node_index = node.index.load(std::memory_order_acquire) - (r + 1);

        if (node_index == 0 && read.val.compare_exchange_strong(r, r + 1, std::memory_order_relaxed))
        {
            val = std::move(node.data);
            node.index.store(r + size, std::memory_order_release);
            return true;
        }
        return false;
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
