// Several producer threads submit orders through the queue to the single
// matching thread. The test checks that every order is processed exactly once
// and that no quantity is created or lost. Run it under ThreadSanitizer too:
//   cmake -B build-tsan -DOB_TSAN=ON && cmake --build build-tsan && ctest --test-dir build-tsan

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "orderbook/MatchingEngine.hpp"
#include "orderbook/OrderQueue.hpp"

using namespace ob;

TEST(OrderQueue, PopReturnsItemsInFifoOrderAndNulloptAfterCloseAndDrain) {
    OrderQueue<int> q(4);
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    q.close();
    EXPECT_FALSE(q.push(3));  // closed queues reject new items
    EXPECT_EQ(q.pop(), 1);    // ...but still hand out what they hold
    EXPECT_EQ(q.pop(), 2);
    EXPECT_EQ(q.pop(), std::nullopt);
}

TEST(OrderQueue, PushBlocksWhileFullUntilAConsumerPops) {
    OrderQueue<int> q(1);
    ASSERT_TRUE(q.push(1));
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        q.push(2);
        pushed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(pushed.load());
    EXPECT_EQ(q.pop(), 1);
    producer.join();
    EXPECT_TRUE(pushed.load());
    EXPECT_EQ(q.pop(), 2);
}

TEST(OrderQueue, CloseWakesABlockedConsumer) {
    OrderQueue<int> q(1);
    std::thread consumer([&] { EXPECT_EQ(q.pop(), std::nullopt); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.close();
    consumer.join();
}

TEST(Concurrency, FourProducersEveryOrderProcessedOnceAndQuantityConserved) {
    constexpr int kProducers = 4;
    constexpr int kOrdersPerProducer = 25000;  // 100,000 in total

    // Build each producer's orders up front. Ids never collide across producers.
    std::vector<std::vector<Command>> work(kProducers);
    std::unordered_map<OrderId, Quantity> submitted;
    for (int p = 0; p < kProducers; ++p) {
        std::mt19937 rng(1000 + p);
        for (int i = 0; i < kOrdersPerProducer; ++i) {
            Command c;
            c.kind = CommandKind::Add;
            c.id = static_cast<OrderId>(p) * 1000000 + static_cast<OrderId>(i) + 1;
            c.side = rng() % 2 ? Side::Buy : Side::Sell;
            c.type = rng() % 10 == 0 ? OrderType::Market : OrderType::Limit;
            c.quantity = 1 + static_cast<Quantity>(rng() % 100);
            c.price = 10000 + static_cast<Price>(rng() % 41) - 20;
            work[p].push_back(c);
            submitted[c.id] = c.quantity;
        }
    }

    // These are only touched on the matching thread, then read after stop().
    std::unordered_map<OrderId, int> timesProcessed;
    std::unordered_map<OrderId, Quantity> filled;
    std::unordered_map<OrderId, Quantity> cancelled;
    int rejected = 0;

    MatchingEngine engine(1024);  // small queue so producers really block
    engine.setTradeListener([&](const Trade& t) {
        filled[t.buyId] += t.quantity;
        filled[t.sellId] += t.quantity;
    });
    engine.setReportListener([&](const Command& c, const ExecutionReport& r) {
        ++timesProcessed[c.id];
        if (!r.accepted) ++rejected;
        if (c.type == OrderType::Market) cancelled[c.id] += r.remaining;
    });
    engine.start();

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (const Command& c : work[p]) ASSERT_TRUE(engine.submit(c));
        });
    }
    for (auto& t : producers) t.join();
    engine.stop();

    const OrderBook& book = engine.book();
    std::string why;
    ASSERT_TRUE(book.checkInvariants(&why)) << why;

    EXPECT_EQ(engine.processed(), static_cast<std::uint64_t>(kProducers * kOrdersPerProducer));
    EXPECT_EQ(rejected, 0);
    ASSERT_EQ(timesProcessed.size(), submitted.size());

    Quantity totalSubmitted = 0, totalFilled = 0, totalResting = 0, totalCancelled = 0;
    for (const auto& [id, qty] : submitted) {
        ASSERT_EQ(timesProcessed[id], 1) << "order " << id;
        const Quantity f = filled.count(id) ? filled[id] : 0;
        const Quantity r = book.restingQuantity(id).value_or(0);
        const Quantity c = cancelled.count(id) ? cancelled[id] : 0;
        ASSERT_EQ(f + r + c, qty) << "order " << id << " filled=" << f << " resting=" << r << " cancelled=" << c;
        totalSubmitted += qty;
        totalFilled += f;
        totalResting += r;
        totalCancelled += c;
    }
    EXPECT_EQ(totalSubmitted, totalFilled + totalResting + totalCancelled);
    EXPECT_EQ(totalResting, book.totalRestingQuantity());
}

TEST(Concurrency, CancelsFromProducersAndCleanShutdown) {
    // One producer adds resting orders, another cancels them. Whatever the
    // interleaving, every cancel either removes an order or is rejected with
    // a reason, and the book is consistent at the end.
    MatchingEngine engine(64);
    std::atomic<int> accepted{0}, rejected{0};
    engine.setReportListener([&](const Command& c, const ExecutionReport& r) {
        if (c.kind != CommandKind::Cancel) return;
        (r.accepted ? accepted : rejected).fetch_add(1);
    });
    engine.start();

    constexpr int kOrders = 5000;
    std::thread adder([&] {
        for (int i = 1; i <= kOrders; ++i) {
            Command c;
            c.id = static_cast<OrderId>(i);
            c.side = Side::Buy;
            c.quantity = 10;
            c.price = 5000 + i % 50;  // all bids, nothing ever matches
            engine.submit(c);
        }
    });
    std::thread canceller([&] {
        for (int i = 1; i <= kOrders; ++i) {
            Command c;
            c.kind = CommandKind::Cancel;
            c.id = static_cast<OrderId>(i);
            engine.submit(c);
        }
    });
    adder.join();
    canceller.join();
    engine.stop();
    engine.stop();  // idempotent

    EXPECT_EQ(accepted + rejected, kOrders);
    EXPECT_EQ(engine.book().orderCount(), static_cast<std::size_t>(rejected.load()));
    std::string why;
    EXPECT_TRUE(engine.book().checkInvariants(&why)) << why;
    EXPECT_FALSE(engine.submit(Command{}));  // stopped engines reject new work
}
