#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "orderbook/CommandParser.hpp"
#include "orderbook/ExecutionReport.hpp"
#include "orderbook/OrderBook.hpp"
#include "orderbook/OrderQueue.hpp"
#include "orderbook/Trade.hpp"

namespace ob {

// Single-writer matching engine.
//
// Any number of producer threads call submit(). Commands go into one bounded
// queue, and exactly one matching thread pops them and applies them to the
// book. Because only that thread touches the book, the book needs no locks,
// and the processing order (one global arrival sequence) is deterministic.
//
// Listeners are called on the matching thread, in processing order.
class MatchingEngine {
public:
    using TradeListener = std::function<void(const Trade&)>;
    using ReportListener = std::function<void(const Command&, const ExecutionReport&)>;
    using BookListener = std::function<void(const OrderBook&)>;  // called for PRINT

    explicit MatchingEngine(std::size_t queueCapacity = 1 << 16);
    ~MatchingEngine();

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Set listeners before start().
    void setTradeListener(TradeListener l) { onTrade_ = std::move(l); }
    void setReportListener(ReportListener l) { onReport_ = std::move(l); }
    void setBookListener(BookListener l) { onBook_ = std::move(l); }

    // When enabled, the matching thread records how long each command took,
    // from the moment it was popped to the moment processing finished.
    void setLatencyRecording(bool on, std::size_t expectedCommands = 0);

    void start();

    // Thread-safe. Blocks while the queue is full. Returns false after stop().
    bool submit(const Command& cmd);

    // Closes the queue, lets the matching thread drain what is left, and joins
    // it. Safe to call more than once.
    void stop();

    // Only safe to read once stop() has returned.
    const OrderBook& book() const { return book_; }
    std::uint64_t processed() const { return processed_.load(std::memory_order_relaxed); }
    const std::vector<std::int64_t>& latenciesNs() const { return latencies_; }

private:
    void run();
    void apply(const Command& cmd);

    OrderQueue<Command> queue_;
    OrderBook book_;
    std::thread thread_;
    std::mutex lifecycle_;
    bool started_ = false;
    bool stopped_ = false;

    TradeListener onTrade_;
    ReportListener onReport_;
    BookListener onBook_;

    bool recordLatency_ = false;
    std::vector<std::int64_t> latencies_;
    std::atomic<std::uint64_t> processed_{0};
};

}  // namespace ob
