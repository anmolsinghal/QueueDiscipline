#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numeric>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// The existing headers use a few identical global helper names.  Private
// namespaces let this one executable benchmark the implementations together.
namespace spsc_benchmark {
#include "spsc/spsc.hpp"
}

namespace spmc_benchmark {
#include "spmc/spmc.hpp"
}

namespace mpsc_benchmark {
#include "mpsc/mpsc.hpp"
}

namespace mpmc_benchmark {
#include "mpmc/mpmc.hpp"
}

using Clock = std::chrono::steady_clock;
using Value = std::uint64_t;
constexpr std::size_t capacity = 1024;

struct Result {
    std::vector<double> mops;

    double percentile(double fraction) const
    {
        auto values = mops;
        std::sort(values.begin(), values.end());
        return values[static_cast<std::size_t>(fraction * (values.size() - 1))];
    }
};

constexpr Value sum_of_range(std::size_t first, std::size_t count)
{
    return static_cast<Value>(count) * (2 * static_cast<Value>(first) + count - 1) / 2;
}

template<typename Queue>
double run_spsc(std::size_t items)
{
    Queue queue;
    std::barrier ready{3};
    std::barrier start{3};
    Value sum = 0;

    std::thread producer([&] {
        ready.arrive_and_wait();
        start.arrive_and_wait();
        for (Value value = 0; value < items; ++value)
            while (!queue.push(value)) {}
    });
    std::thread consumer([&] {
        Value value;
        ready.arrive_and_wait();
        start.arrive_and_wait();
        for (std::size_t i = 0; i < items; ++i) {
            while (!queue.pop(value)) {}
            sum += value;
        }
    });

    ready.arrive_and_wait();
    const auto begin = Clock::now();
    start.arrive_and_wait();
    producer.join();
    consumer.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();

    if (sum != sum_of_range(0, items))
        std::terminate();
    return items / elapsed / 1e6;
}

template<typename Queue>
double run_spmc(std::size_t consumers, std::size_t items)
{
    Queue queue;
    std::barrier ready{static_cast<std::ptrdiff_t>(consumers + 2)};
    std::barrier start{static_cast<std::ptrdiff_t>(consumers + 2)};
    std::atomic<bool> producer_done{false};
    std::vector<std::size_t> counts(consumers);
    std::vector<Value> sums(consumers);
    std::vector<std::thread> consumer_threads;
    consumer_threads.reserve(consumers);

    for (std::size_t id = 0; id < consumers; ++id) {
        consumer_threads.emplace_back([&, id] {
            std::size_t count = 0;
            Value sum = 0;
            Value value = 0;
            ready.arrive_and_wait();
            start.arrive_and_wait();

            while (true) {
                if (queue.pop(value)) {
                    ++count;
                    sum += value;
                    continue;
                }

                if (producer_done.load(std::memory_order_acquire)) {
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
    std::thread producer([&] {
        ready.arrive_and_wait();
        start.arrive_and_wait();
        for (Value value = 0; value < items; ++value)
            while (!queue.push(value)) {}
        producer_done.store(true, std::memory_order_release);
    });

    ready.arrive_and_wait();
    const auto begin = Clock::now();
    start.arrive_and_wait();
    producer.join();
    for (auto& thread : consumer_threads)
        thread.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();

    if (std::accumulate(counts.begin(), counts.end(), std::size_t{0}) != items ||
        std::accumulate(sums.begin(), sums.end(), Value{0}) != sum_of_range(0, items))
        std::terminate();
    return static_cast<double>(items) / elapsed / 1e6;
}

template<typename Queue>
double run_mpsc(std::size_t producers, std::size_t items_per_producer)
{
    Queue queue;
    const std::size_t items = producers * items_per_producer;
    std::barrier ready{static_cast<std::ptrdiff_t>(producers + 2)};
    std::barrier start{static_cast<std::ptrdiff_t>(producers + 2)};
    Value sum = 0;
    std::vector<std::thread> producer_threads;
    producer_threads.reserve(producers);

    for (std::size_t id = 0; id < producers; ++id) {
        producer_threads.emplace_back([&, id] {
            const Value first = id * items_per_producer;
            ready.arrive_and_wait();
            start.arrive_and_wait();
            for (Value value = first; value < first + items_per_producer; ++value)
                queue.push(value);
        });
    }
    std::thread consumer([&] {
        Value value;
        ready.arrive_and_wait();
        start.arrive_and_wait();
        for (std::size_t i = 0; i < items; ++i) {
            queue.pop(value);
            sum += value;
        }
    });

    ready.arrive_and_wait();
    const auto begin = Clock::now();
    start.arrive_and_wait();
    for (auto& thread : producer_threads)
        thread.join();
    consumer.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();

    if (sum != sum_of_range(0, items))
        std::terminate();
    return items / elapsed / 1e6;
}

template<typename Queue>
double run_mpmc(std::size_t producers, std::size_t consumers, std::size_t items_per_producer)
{
    Queue queue;
    const std::size_t items = producers * items_per_producer;
    std::barrier ready{static_cast<std::ptrdiff_t>(producers + consumers + 1)};
    std::barrier start{static_cast<std::ptrdiff_t>(producers + consumers + 1)};
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
            start.arrive_and_wait();

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
            start.arrive_and_wait();
            for (Value value = first; value < first + items_per_producer; ++value)
                while (!queue.push(value)) {}
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    ready.arrive_and_wait();
    const auto begin = Clock::now();
    start.arrive_and_wait();
    for (auto& thread : threads)
        thread.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();

    if (std::accumulate(counts.begin(), counts.end(), std::size_t{0}) != items ||
        std::accumulate(sums.begin(), sums.end(), Value{0}) != sum_of_range(0, items))
        std::terminate();
    return static_cast<double>(items) / elapsed / 1e6;
}

template<typename Run>
Result measure(Run&& run, std::size_t trials)
{
    run(); // Warm-up outside the reported results.
    Result result;
    result.mops.reserve(trials);
    for (std::size_t i = 0; i < trials; ++i)
        result.mops.push_back(run());
    return result;
}

void print_result(const std::string& label, const Result& result)
{
    std::cout << std::left << std::setw(21) << label
              << " median " << std::setw(8) << std::fixed << std::setprecision(2) << result.percentile(.50)
              << " M items/s  p10 " << std::setw(8) << result.percentile(.10)
              << "  p90 " << std::setw(8) << result.percentile(.90)
              << "  best " << std::setw(8) << *std::max_element(result.mops.begin(), result.mops.end()) << '\n';
}

int main(int argc, char* argv[])
{
    const std::size_t items = argc > 1 ? std::stoull(argv[1]) : 5'000'000;
    const std::size_t trials = argc > 2 ? std::stoull(argv[2]) : 9;

    if (items == 0 || trials == 0 || items % 8 != 0) {
        std::cerr << "items must be a non-zero multiple of 8; trials must be non-zero\n";
        return 1;
    }

    std::cout << "uint64_t payload, capacity " << capacity << ", " << items
              << " total items, " << trials << " measured trials plus one warm-up\n";
    std::cout << "Every trial has a synchronized start and validates a value checksum.\n\n";

    print_result("SPSC (1P / 1C)", measure([&] {
        return run_spsc<spsc_benchmark::SPSCQueue<Value, capacity>>(items);
    }, trials));

    for (std::size_t consumers : {1u, 2u, 4u, 8u}) {
        print_result("SPMC (1P / " + std::to_string(consumers) + "C)", measure([&] {
            return run_spmc<spmc_benchmark::SPMCQueue<Value, capacity>>(consumers, items);
        }, trials));
    }

    for (std::size_t producers : {1u, 2u, 4u, 8u}) {
        const std::size_t per_producer = items / producers;
        print_result("MPSC (" + std::to_string(producers) + "P / 1C)", measure([&] {
            return run_mpsc<mpsc_benchmark::mpsc<Value>>(producers, per_producer);
        }, trials));
    }

    for (std::size_t workers : {1u, 2u, 4u, 8u}) {
        const std::size_t per_producer = items / workers;
        print_result("MPMC (" + std::to_string(workers) + "P / " +
                         std::to_string(workers) + "C)", measure([&] {
            return run_mpmc<mpmc_benchmark::MPMC<Value, capacity>>(
                workers, workers, per_producer);
        }, trials));
    }
}
