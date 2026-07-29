#include <iostream>
#include <string>
#include <string_view>
#include <atomic>
#include <array>
#include <thread>
#include <chrono>
#include <utility>
#include <concepts>
#include <cstddef>
#include <new>

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
class lockfreequeue
{   
    struct alignas(hardware_destructive_interference_size) cacheline_atomic {
        std::atomic<std::size_t> val{0};
    };
    static constexpr std::size_t mask = size - 1; // for fast wraparound

    alignas(hardware_destructive_interference_size) cacheline_atomic read;
    std::size_t read_cache{0};

    alignas(hardware_destructive_interference_size) cacheline_atomic write;
    std::size_t write_cache{0};

    alignas(hardware_destructive_interference_size) std::array<T, size> data;

    public:

    lockfreequeue() : read{}, write{} {}

    // Pass-by-reference pop
    bool pop(T& val)
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

    template<typename... Args>
    bool emplace(Args&&... args)
    {
        auto w = write.val.load(std::memory_order_relaxed);

        if (w - read_cache == size)
        {
            read_cache = read.val.load(std::memory_order_acquire);
            if (w - read_cache == size)
                return false;
        }

        data[w & mask] = T(std::forward<Args>(args)...);
        write.val.store(w + 1, std::memory_order_release);
        return true;
    }

    template<typename U>
    bool push(U&& val)
    {
        return emplace(std::forward<U>(val));
    }
};

int main()
{
    constexpr int N = 10'000'000;
    lockfreequeue<int, 1024> q;
    std::atomic<bool> ok{true};

    auto producer = [&]() {
        for (int i = 0; i < N; i++) {
            while (!q.push(i)) {}
        }
    };

    auto consumer = [&]() {
        int cur = 0;
        int val = 0;
        while (cur < N)
        {
            if (q.pop(val))
            {   
                if (val == cur) {
                    cur++;
                } else {
                    ok.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        }
    };

    auto start = std::chrono::high_resolution_clock::now();

    std::thread t1(producer);
    std::thread t2(consumer);

    t1.join();
    t2.join();
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    if (ok.load())
        std::cout << "Correctness check passed!\n";
    else
        std::cout << "Data mismatch detected!\n";

    std::cout << "Transferred " << N << " items in "
              << diff.count() << " seconds\n";
    std::cout << "Throughput: " << (N / diff.count()) / 1e6
              << " million msgs/sec\n";
    std::cout << "Average latency per msg: " << (diff.count() / N) * 1e9
              << " ns\n";
}