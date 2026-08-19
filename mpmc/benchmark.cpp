#include "mpmc.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using Value = std::uint64_t;
constexpr std::size_t capacity = 1024;

constexpr Value sum_of_range(std::size_t count)
{
    return static_cast<Value>(count) * (count - 1) / 2;
}

template<std::size_t Capacity>
void run_mpmc_benchmark(std::size_t producers, std::size_t consumers,
                        std::size_t items_per_producer)
{
    MPMC<Value, Capacity> queue;
    const std::size_t items = producers * items_per_producer;
    std::barrier ready{static_cast<std::ptrdiff_t>(producers + consumers + 1)};
    std::atomic<std::size_t> producers_done{0};
    std::vector<std::size_t> counts(consumers);
    std::vector<Value> sums(consumers);
    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (std::size_t id = 0; id < consumers; ++id) {
        threads.emplace_back([&, id] {
            std::size_t count = 0;
            Value sum = 0;
            Value value = 0;
            ready.arrive_and_wait();

            while (true) {
                if (queue.pop(value)) {
                    ++count;
                    sum += value;
                    continue;
                }

                if (producers_done.load(std::memory_order_acquire) == producers) {
                    if (queue.pop(value)) {
                        ++count;
                        sum += value;
                        continue;
                    }
                    break;
                }
            }

            counts[id] = count;
            sums[id] = sum;
        });
    }

    for (std::size_t id = 0; id < producers; ++id) {
        threads.emplace_back([&, id] {
            const Value first = id * items_per_producer;
            ready.arrive_and_wait();
            for (Value value = first; value < first + items_per_producer; ++value)
                while (!queue.push(value)) {}
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    const auto begin = Clock::now();
    ready.arrive_and_wait();
    for (auto& thread : threads)
        thread.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();

    const std::size_t count =
        std::accumulate(counts.begin(), counts.end(), std::size_t{0});
    const Value sum = std::accumulate(sums.begin(), sums.end(), Value{0});
    if (count != items || sum != sum_of_range(items))
        std::terminate();

    const double item_count = static_cast<double>(items);
    const double mops = item_count / elapsed / 1e6;
    const double ns_per_item = elapsed / item_count * 1e9;
    const std::string workers = std::to_string(producers) + "P / " +
                                std::to_string(consumers) + "C";
    std::cout << std::left << std::setw(12) << workers
              << " | " << std::right << std::setw(10) << std::fixed
              << std::setprecision(2) << mops << " M items/s"
              << " | " << std::setw(8) << ns_per_item << " ns/item\n";
}

int main(int argc, char* argv[])
{
    const std::size_t items = argc > 1 ? std::stoull(argv[1]) : 8'000'000;
    if (items == 0 || items % 8 != 0) {
        std::cerr << "items must be a non-zero multiple of 8\n";
        return 1;
    }

    std::cout << "========================================================\n";
    std::cout << "         MPMC LOCK-FREE QUEUE BENCHMARK SUITE\n";
    std::cout << "========================================================\n";
    std::cout << "uint64_t payload, capacity " << capacity << ", " << items
              << " total items per run; every run validates count and checksum.\n\n";

    for (std::size_t workers : {1u, 2u, 4u, 8u})
        run_mpmc_benchmark<capacity>(workers, workers, items / workers);
}
