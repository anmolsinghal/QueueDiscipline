#include "spmc.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <cstdint>
#include <atomic>
#include <string>

using Clock = std::chrono::high_resolution_clock;

// Throughput benchmark with configurable consumer count
template<typename T, std::size_t Capacity>
void run_spmc_throughput_benchmark(std::size_t num_consumers, std::size_t num_items) {
    SPMCQueue<T, Capacity> q;
    std::atomic<bool> producer_done{false};
    std::vector<std::size_t> consumer_counts(num_consumers, 0);

    auto start = Clock::now();

    // Spawn consumer threads
    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (std::size_t c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&, c]() {
            std::size_t count = 0;
            T item{};
            while (true) {
                if (q.pop(item)) {
                    ++count;
                } else if (producer_done.load(std::memory_order_acquire)) {
                    // Double check if any remaining items exist
                    if (q.pop(item)) {
                        ++count;
                    } else {
                        break;
                    }
                }
            }
            consumer_counts[c] = count;
        });
    }

    // Producer thread
    std::thread producer([&]() {
        T item{};
        for (std::size_t i = 0; i < num_items; ++i) {
            while (!q.push(item)) {
                #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
                #endif
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    for (auto& consumer : consumers) {
        consumer.join();
    }

    auto end = Clock::now();

    std::size_t total_popped = 0;
    for (std::size_t c = 0; c < num_consumers; ++c) {
        total_popped += consumer_counts[c];
    }

    double seconds = std::chrono::duration<double>(end - start).count();
    double m_ops = (total_popped / seconds) / 1e6;
    double avg_ns = (seconds / total_popped) * 1e9;

    std::cout << std::left << std::setw(12) << (std::to_string(num_consumers) + " Consumers")
              << " | " << std::right << std::setw(10) << std::fixed << std::setprecision(2) << m_ops << " M ops/s"
              << " | " << std::setw(8) << std::fixed << std::setprecision(2) << avg_ns << " ns/op"
              << " | Total Popped: " << total_popped << "\n";

    // Print work distribution across consumers
    std::cout << "   Work Distribution: [";
    for (std::size_t c = 0; c < num_consumers; ++c) {
        double pct = (double)consumer_counts[c] / total_popped * 100.0;
        std::cout << "C" << c << ": " << std::fixed << std::setprecision(1) << pct << "%" 
                  << (c + 1 < num_consumers ? ", " : "");
    }
    std::cout << "]\n\n";
}

int main(int argc, char* argv[]) {
    std::cout << "========================================================\n";
    std::cout << "         SPMC LOCK-FREE QUEUE BENCHMARK SUITE\n";
    std::cout << "========================================================\n\n";

    std::size_t num_items = 10'000'000;
    std::vector<std::size_t> consumer_configs = {1, 2, 4, 8};

    // Allow overriding consumer count from command line arguments
    if (argc > 1) {
        std::size_t custom_consumers = std::stoull(argv[1]);
        consumer_configs = { custom_consumers };
        std::cout << "Running benchmark with custom consumer count: " << custom_consumers << "\n\n";
    }

    if (argc > 2) {
        num_items = std::stoull(argv[2]);
    }

    std::cout << "--- THROUGHPUT BENCHMARK (" << num_items << " operations) ---\n";
    for (std::size_t consumers : consumer_configs) {
        run_spmc_throughput_benchmark<uint64_t, 1024>(consumers, num_items);
    }

    return 0;
}
