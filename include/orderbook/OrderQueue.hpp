#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ob {

// A bounded, blocking, multi-producer multi-consumer queue.
//
// push() blocks while the queue is full; pop() blocks while it is empty.
// close() wakes everyone up: after it, push() fails and pop() keeps returning
// items until the queue is drained, then returns nullopt. That is what lets
// the matching thread finish the remaining work and exit cleanly.
template <class T>
class OrderQueue {
public:
    explicit OrderQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    OrderQueue(const OrderQueue&) = delete;
    OrderQueue& operator=(const OrderQueue&) = delete;

    // Returns false if the queue was closed (the item is not enqueued).
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!closed_ && items_.size() >= capacity_) {
            ++waitingProducers_;
            notFull_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
            --waitingProducers_;
        }
        if (closed_) return false;
        items_.push_back(std::move(item));
        // Only pay for a notify when the consumer is actually asleep.
        const bool wake = waitingConsumers_ > 0;
        lock.unlock();
        if (wake) notEmpty_.notify_one();
        return true;
    }

    // Returns nullopt only once the queue is closed and empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        waitForItems(lock);
        if (items_.empty()) return std::nullopt;  // closed and drained
        T item = std::move(items_.front());
        items_.pop_front();
        const bool wake = waitingProducers_ > 0;
        lock.unlock();
        if (wake) notFull_.notify_all();
        return item;
    }

    // Moves up to `max` items into `out` (which is cleared first) under a
    // single lock acquisition. Blocks while the queue is empty and open.
    // Returns false only once the queue is closed and drained.
    //
    // Taking a batch per lock is what makes the queue fast: the consumer
    // contends for the mutex once per batch instead of once per order.
    bool popBatch(std::vector<T>& out, std::size_t max) {
        out.clear();
        std::unique_lock<std::mutex> lock(mutex_);
        waitForItems(lock);
        if (items_.empty()) return false;  // closed and drained
        while (!items_.empty() && out.size() < max) {
            out.push_back(std::move(items_.front()));
            items_.pop_front();
        }
        const bool wake = waitingProducers_ > 0;
        lock.unlock();
        if (wake) notFull_.notify_all();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    std::size_t capacity() const { return capacity_; }

private:
    void waitForItems(std::unique_lock<std::mutex>& lock) {
        if (closed_ || !items_.empty()) return;
        ++waitingConsumers_;
        notEmpty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        --waitingConsumers_;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> items_;
    bool closed_ = false;
    std::size_t waitingProducers_ = 0;
    std::size_t waitingConsumers_ = 0;
};

}  // namespace ob
