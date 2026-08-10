#include "spsc.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <cstdint>

// Struct payloads for size testing
struct alignas(64) Payload64 {
    uint64_t data[8];
};

struct alignas(64) Payload512 {
    uint64_t data[64];
};

using Clock = std::chrono::high_resolution_clock;

// 1. Throughput Benchmark
template<typename T, std::size_t Capacity>
void run_throughput_benchmark(const std::string& label, std::size_t num_items) {
    SPSCQueue<T, Capacity> q;

    auto start = Clock::now();

    std::thread producer([&]() {
        T item{};
        for (std::size_t i = 0; i < num_items; ++i) {
            while (!q.push(item)) {}
        }
    });

    std::thread consumer([&]() {
        T item{};
        for (std::size_t i = 0; i < num_items; ++i) {
            while (!q.pop(item)) {}
        }
    });

    producer.join();
    consumer.join();

    auto end = Clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();
    double m_ops = (num_items / seconds) / 1e6;
    double avg_ns = (seconds / num_items) * 1e9;

    std::cout << std::left << std::setw(28) << label
              << " | " << std::right << std::setw(10) << std::fixed << std::setprecision(2) << m_ops << " M ops/s"
              << " | " << std::setw(8) << std::fixed << std::setprecision(2) << avg_ns << " ns/op\n";
}

// 2. Ping-Pong Round-Trip Latency Benchmark (Collects Percentiles)
template<std::size_t Capacity>
void run_latency_benchmark(std::size_t num_samples) {
    SPSCQueue<uint64_t, Capacity> q1; // Producer -> Consumer
    SPSCQueue<uint64_t, Capacity> q2; // Consumer -> Producer

    std::vector<double> latencies_ns(num_samples);
    constexpr std::size_t WARMUP = 10000;

    std::thread consumer([&]() {
        uint64_t val = 0;
        for (std::size_t i = 0; i < WARMUP + num_samples; ++i) {
            while (!q1.pop(val)) {}
            while (!q2.push(val)) {}
        }
    });

    std::thread producer([&]() {
        uint64_t val = 42;
        // Warmup iterations
        for (std::size_t i = 0; i < WARMUP; ++i) {
            while (!q1.push(val)) {}
            while (!q2.pop(val)) {}
        }

        for (std::size_t i = 0; i < num_samples; ++i) {
            auto t0 = Clock::now();
            while (!q1.push(val)) {}
            while (!q2.pop(val)) {}
            auto t1 = Clock::now();
            
            // Half of round-trip time gives single-direction latency estimate
            double rtt_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
            latencies_ns[i] = rtt_ns / 2.0; 
        }
    });

    producer.join();
    consumer.join();

    std::sort(latencies_ns.begin(), latencies_ns.end());

    auto p50  = latencies_ns[static_cast<std::size_t>(num_samples * 0.50)];
    auto p90  = latencies_ns[static_cast<std::size_t>(num_samples * 0.90)];
    auto p95  = latencies_ns[static_cast<std::size_t>(num_samples * 0.95)];
    auto p99  = latencies_ns[static_cast<std::size_t>(num_samples * 0.99)];
    auto p999 = latencies_ns[static_cast<std::size_t>(num_samples * 0.999)];
    auto max  = latencies_ns.back();

    std::cout << "\n========================================================\n";
    std::cout << "         ROUND-TRIP LATENCY PERCENTILES (ns)\n";
    std::cout << "========================================================\n";
    std::cout << "  p50   (median) : " << std::fixed << std::setprecision(2) << p50  << " ns\n";
    std::cout << "  p90            : " << std::fixed << std::setprecision(2) << p90  << " ns\n";
    std::cout << "  p95            : " << std::fixed << std::setprecision(2) << p95  << " ns\n";
    std::cout << "  p99            : " << std::fixed << std::setprecision(2) << p99  << " ns\n";
    std::cout << "  p99.9          : " << std::fixed << std::setprecision(2) << p999 << " ns\n";
    std::cout << "  Max            : " << std::fixed << std::setprecision(2) << max  << " ns\n";
    std::cout << "========================================================\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "         SPSC LOCK-FREE QUEUE BENCHMARK SUITE\n";
    std::cout << "========================================================\n";
    
    constexpr std::size_t NUM_ITEMS = 20'000'000;
    
    std::cout << "\n--- 1. THROUGHPUT BENCHMARK (" << NUM_ITEMS << " ops) ---\n";
    run_throughput_benchmark<uint64_t, 1024>("int64_t (8B) [Cap=1024]", NUM_ITEMS);
    run_throughput_benchmark<Payload64, 1024>("Payload64 (64B) [Cap=1024]", NUM_ITEMS);
    run_throughput_benchmark<Payload512, 1024>("Payload512 (512B) [Cap=1024]", NUM_ITEMS);

    std::cout << "\n--- 2. CAPACITY SENSITIVITY BENCHMARK ---\n";
    run_throughput_benchmark<uint64_t, 128>("int64_t [Cap=128]", NUM_ITEMS);
    run_throughput_benchmark<uint64_t, 1024>("int64_t [Cap=1024]", NUM_ITEMS);
    run_throughput_benchmark<uint64_t, 8192>("int64_t [Cap=8192]", NUM_ITEMS);
    run_throughput_benchmark<uint64_t, 65536>("int64_t [Cap=65536]", NUM_ITEMS);

    std::cout << "\n--- 3. LATENCY DISTRIBUTION BENCHMARK (1,000,000 samples) ---\n";
    run_latency_benchmark<1024>(1'000'000);

    return 0;
}
