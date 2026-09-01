#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

// Unbounded multiple-producer/single-consumer queue.
//
// Multiple threads may call push(); exactly one thread may call pop(). push()
// allocates one node. pop() spins until an item is available and therefore does
// not provide an empty result. All participating threads must stop before the
// queue is destroyed. T must be default-constructible, copy-constructible, and
// move-assignable.
template<typename T>
struct mpsc
{
    struct Node
    {
        T data;
        std::atomic<Node*> next{nullptr};
    };

#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t cacheline_size =
        std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t cacheline_size = 64;
#endif

    mpsc() : head{new Node()}, tail{head} {}

    mpsc(const mpsc&) = delete;
    mpsc& operator=(const mpsc&) = delete;
    mpsc(mpsc&&) = delete;
    mpsc& operator=(mpsc&&) = delete;

    ~mpsc()
    {
        // All producers and the consumer must be stopped before destruction.
        while (head != nullptr)
        {
            Node* next = head->next.load(std::memory_order_relaxed);
            delete head;
            head = next;
        }
    }

    void pop(T& val)
    {
        Node* n;
        do
        {
            n = head->next.load(std::memory_order_acquire);
        }
        while (n == nullptr);

        val = std::move(n->data);
        Node* old_head = head;
        head = n;
        delete old_head;
    }

    void push(const T& val)
    {
        Node* n = new Node{val, nullptr};

        Node* previous = tail.exchange(n, std::memory_order_acq_rel);

        previous->next.store(n, std::memory_order_release);
    }

    alignas(cacheline_size) Node* head;
    alignas(cacheline_size) std::atomic<Node*> tail;

};
