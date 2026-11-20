#include "orderbook/MatchingEngine.hpp"

#include <chrono>

namespace ob {

MatchingEngine::MatchingEngine(std::size_t queueCapacity) : queue_(queueCapacity) {}

MatchingEngine::~MatchingEngine() { stop(); }

void MatchingEngine::setLatencyRecording(bool on, std::size_t expectedCommands) {
    recordLatency_ = on;
    latencies_.clear();
    if (on) latencies_.reserve(expectedCommands);
}

void MatchingEngine::start() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (started_) return;
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

bool MatchingEngine::submit(const Command& cmd) { return queue_.push(cmd); }

void MatchingEngine::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (stopped_) return;
    stopped_ = true;
    queue_.close();  // producers can no longer push; the consumer drains the rest
    if (thread_.joinable()) thread_.join();
}

void MatchingEngine::run() {
    using Clock = std::chrono::steady_clock;
    while (auto cmd = queue_.pop()) {
        if (recordLatency_) {
            const auto t0 = Clock::now();
            apply(*cmd);
            const auto t1 = Clock::now();
            latencies_.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        } else {
            apply(*cmd);
        }
        processed_.fetch_add(1, std::memory_order_relaxed);
    }
}

void MatchingEngine::apply(const Command& cmd) {
    ExecutionReport report;
    switch (cmd.kind) {
        case CommandKind::Add:
            report = cmd.type == OrderType::Limit ? book_.addLimit(cmd.id, cmd.side, cmd.quantity, cmd.price)
                                                  : book_.addMarket(cmd.id, cmd.side, cmd.quantity);
            break;
        case CommandKind::Cancel:
            report = book_.cancel(cmd.id);
            break;
        case CommandKind::Print:
            if (onBook_) onBook_(book_);
            return;
        case CommandKind::Quit:
            return;
    }
    if (onTrade_) {
        for (const Trade& t : report.trades) onTrade_(t);
    }
    if (onReport_) onReport_(cmd, report);
}

}  // namespace ob
