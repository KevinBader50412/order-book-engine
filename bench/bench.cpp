// Throughput and latency benchmark.
//
//   ./bench --orders 1000000 --producers 4 --seed 42
//
// Generates a reproducible stream of orders around a mid price of 100.00:
// 70% limit orders, 20% cancels of orders the producer placed earlier, and
// 10% market orders. All orders are generated before timing starts.
//
// Two runs over the same commands:
//   1. Multithreaded: producers push into the queue, one matching thread
//      applies them. Latency is measured on the matching thread from the pop
//      to the end of processing.
//   2. Single-threaded baseline: the same commands, interleaved round-robin,
//      applied straight to an OrderBook with no queue.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "orderbook/MatchingEngine.hpp"
#include "orderbook/OrderBook.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::size_t orders = 1000000;
    int producers = 4;
    std::uint64_t seed = 42;
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--orders")) o.orders = std::stoull(next("--orders"));
        else if (!std::strcmp(argv[i], "--producers")) o.producers = std::stoi(next("--producers"));
        else if (!std::strcmp(argv[i], "--seed")) o.seed = std::stoull(next("--seed"));
        else {
            std::cerr << "usage: " << argv[0] << " [--orders N] [--producers P] [--seed S]\n";
            std::exit(2);
        }
    }
    if (o.producers < 1) o.producers = 1;
    return o;
}

// Each producer gets its own generator seeded from the global seed, and its
// own id range, so runs are reproducible and ids never collide.
std::vector<Command> generate(const Options& opt, int producer, std::size_t count) {
    std::mt19937_64 rng(opt.seed * 1000003 + static_cast<std::uint64_t>(producer));
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<Quantity> qty(1, 100);
    std::uniform_int_distribution<Price> buyOffset(-60, 20);   // mostly below mid
    std::uniform_int_distribution<Price> sellOffset(-20, 60);  // mostly above mid
    constexpr Price kMid = 10000;                               // 100.00

    std::vector<Command> out;
    out.reserve(count);
    std::vector<OrderId> live;  // limit orders this producer placed and has not cancelled yet
    OrderId nextId = static_cast<OrderId>(producer) << 40;

    for (std::size_t i = 0; i < count; ++i) {
        Command c;
        int roll = pct(rng);
        if (roll < 20 && !live.empty()) {
            // Cancel one of our earlier limit orders. It may have been filled
            // in the meantime, in which case the book rejects the cancel.
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            std::size_t k = pick(rng);
            c.kind = CommandKind::Cancel;
            c.id = live[k];
            live[k] = live.back();
            live.pop_back();
        } else if (roll < 30) {
            c.kind = CommandKind::Add;
            c.type = OrderType::Market;
            c.id = ++nextId;
            c.side = rng() & 1 ? Side::Buy : Side::Sell;
            c.quantity = qty(rng);
        } else {
            c.kind = CommandKind::Add;
            c.type = OrderType::Limit;
            c.id = ++nextId;
            c.side = rng() & 1 ? Side::Buy : Side::Sell;
            c.quantity = qty(rng);
            c.price = kMid + (c.side == Side::Buy ? buyOffset(rng) : sellOffset(rng));
            live.push_back(c.id);
        }
        out.push_back(c);
    }
    return out;
}

struct Result {
    double seconds = 0;
    std::size_t commands = 0;
    std::uint64_t trades = 0;
    std::uint64_t rejects = 0;
    std::vector<std::int64_t> latencies;
};

double percentileUs(std::vector<std::int64_t>& v, double p) {
    if (v.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
    return static_cast<double>(v[idx]) / 1000.0;
}

void report(const char* name, Result& r) {
    const double throughput = static_cast<double>(r.commands) / r.seconds;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << name << '\n'
              << "  commands      " << r.commands << '\n'
              << "  trades        " << r.trades << '\n'
              << "  rejects       " << r.rejects << " (cancels of orders already filled)\n"
              << "  elapsed       " << r.seconds * 1000.0 << " ms\n"
              << "  throughput    " << std::setprecision(0) << throughput << " orders/s\n"
              << std::setprecision(2);
    if (!r.latencies.empty()) {
        std::int64_t maxNs = *std::max_element(r.latencies.begin(), r.latencies.end());
        std::cout << "  latency p50   " << percentileUs(r.latencies, 50) << " us\n"
                  << "  latency p99   " << percentileUs(r.latencies, 99) << " us\n"
                  << "  latency p99.9 " << percentileUs(r.latencies, 99.9) << " us\n"
                  << "  latency max   " << static_cast<double>(maxNs) / 1000.0 << " us\n";
    }
}

Result runMultithreaded(const std::vector<std::vector<Command>>& work, std::size_t total) {
    Result r;
    MatchingEngine engine;
    engine.setLatencyRecording(true, total);
    std::uint64_t trades = 0, rejects = 0;  // only touched on the matching thread
    engine.setReportListener([&](const Command&, const ExecutionReport& rep) {
        trades += rep.trades.size();
        if (!rep.accepted) ++rejects;
    });
    engine.start();

    const auto t0 = Clock::now();
    std::vector<std::thread> producers;
    for (const auto& cmds : work) {
        producers.emplace_back([&engine, &cmds] {
            for (const Command& c : cmds) engine.submit(c);
        });
    }
    for (auto& t : producers) t.join();
    engine.stop();  // waits until the matching thread has drained the queue
    const auto t1 = Clock::now();

    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.commands = engine.processed();
    r.trades = trades;
    r.rejects = rejects;
    r.latencies = engine.latenciesNs();
    return r;
}

Result runSingleThreaded(const std::vector<std::vector<Command>>& work, std::size_t total) {
    // Interleave producers round-robin to get one sequential stream.
    std::vector<Command> stream;
    stream.reserve(total);
    for (std::size_t i = 0;; ++i) {
        bool any = false;
        for (const auto& cmds : work) {
            if (i < cmds.size()) {
                stream.push_back(cmds[i]);
                any = true;
            }
        }
        if (!any) break;
    }

    Result r;
    r.latencies.reserve(stream.size());
    OrderBook book;
    const auto t0 = Clock::now();
    for (const Command& c : stream) {
        const auto s = Clock::now();
        ExecutionReport rep = c.kind == CommandKind::Cancel ? book.cancel(c.id)
                              : c.type == OrderType::Limit  ? book.addLimit(c.id, c.side, c.quantity, c.price)
                                                            : book.addMarket(c.id, c.side, c.quantity);
        const auto e = Clock::now();
        r.latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(e - s).count());
        r.trades += rep.trades.size();
        if (!rep.accepted) ++r.rejects;
    }
    const auto t1 = Clock::now();
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.commands = stream.size();
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = parseArgs(argc, argv);

    std::vector<std::vector<Command>> work;
    const std::size_t per = opt.orders / static_cast<std::size_t>(opt.producers);
    for (int p = 0; p < opt.producers; ++p) {
        std::size_t n = per + (p == opt.producers - 1 ? opt.orders % static_cast<std::size_t>(opt.producers) : 0);
        work.push_back(generate(opt, p, n));
    }

    std::cout << "orders=" << opt.orders << " producers=" << opt.producers << " seed=" << opt.seed
              << " hw_threads=" << std::thread::hardware_concurrency() << "\n\n";

    Result mt = runMultithreaded(work, opt.orders);
    report("multithreaded (producers -> queue -> 1 matching thread)", mt);
    std::cout << '\n';
    Result st = runSingleThreaded(work, opt.orders);
    report("single-threaded baseline (direct calls, no queue)", st);
    return 0;
}
