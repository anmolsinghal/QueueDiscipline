#include "spsc.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

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