#pragma once

#include <atomic>
#include <utility>

template<typename T>
struct mpsc
{
    struct Node
    {
        T data;
        std::atomic<Node*> next{nullptr};
    };

    mpsc() : head(new Node()), tail{head} {}

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

    bool pop(T& val)
    {
        Node* n = head->next.load(std::memory_order_acquire);
        if(n == nullptr)
            return false;
        val = std::move(n->data);
        Node* old_head = head;
        head = n;
        delete old_head;
        return true;
    }

    bool push(const T& val)
    {
        Node* n = new Node{val, nullptr};

        Node* previous = tail.exchange(n, std::memory_order_acq_rel);

        previous->next.store(n, std::memory_order_release);
        return true;
    }
    Node* head;
    std::atomic<Node*> tail;

};
