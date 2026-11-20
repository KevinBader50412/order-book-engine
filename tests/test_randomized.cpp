// Differential test: run the same random command stream through the real
// order book and through a deliberately simple reference implementation, and
// require identical results for every command.
//
// The reference keeps every resting order in one flat vector and scans all
// of it to find the best price each time. It is slow, but it is short enough
// to check by eye, which is the point.

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <random>
#include <unordered_set>
#include <vector>

#include "orderbook/OrderBook.hpp"

using namespace ob;

namespace {

class ReferenceBook {
public:
    ExecutionReport add(OrderId id, Side side, OrderType type, Quantity qty, Price price) {
        if (qty <= 0) return ExecutionReport::reject(id, RejectReason::InvalidQuantity);
        if (seen_.count(id)) return ExecutionReport::reject(id, RejectReason::DuplicateId);
        if (type == OrderType::Limit && price <= 0) return ExecutionReport::reject(id, RejectReason::InvalidPrice);
        seen_.insert(id);

        ExecutionReport r;
        r.id = id;
        r.accepted = true;
        Quantity left = qty;
        while (left > 0) {
            // Find the best opposite order: best price, then lowest sequence.
            std::optional<std::size_t> best;
            for (std::size_t i = 0; i < orders_.size(); ++i) {
                const Order& o = orders_[i];
                if (o.side == side) continue;
                if (!best) { best = i; continue; }
                const Order& b = orders_[*best];
                bool better = side == Side::Buy ? o.price < b.price : o.price > b.price;
                if (better || (o.price == b.price && o.sequence < b.sequence)) best = i;
            }
            if (!best) break;
            Order& o = orders_[*best];
            if (type == OrderType::Limit) {
                bool crosses = side == Side::Buy ? o.price <= price : o.price >= price;
                if (!crosses) break;
            }
            Quantity q = std::min(left, o.quantity);
            r.trades.push_back(side == Side::Buy ? Trade{id, o.id, o.price, q} : Trade{o.id, id, o.price, q});
            left -= q;
            o.quantity -= q;
            r.filled += q;
            if (o.quantity == 0) orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(*best));
        }
        if (type == OrderType::Limit && left > 0) orders_.push_back(Order{id, side, type, left, price, seq_++});
        else ++seq_;
        r.remaining = left;
        return r;
    }

    ExecutionReport cancel(OrderId id) {
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            if (orders_[i].id == id) {
                ExecutionReport r;
                r.id = id;
                r.accepted = true;
                r.remaining = orders_[i].quantity;
                orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
                return r;
            }
        }
        return ExecutionReport::reject(id, seen_.count(id) ? RejectReason::NotActive : RejectReason::UnknownId);
    }

    std::optional<Price> best(Side side) const {
        std::optional<Price> p;
        for (const Order& o : orders_) {
            if (o.side != side) continue;
            if (!p || (side == Side::Buy ? o.price > *p : o.price < *p)) p = o.price;
        }
        return p;
    }

    std::size_t size() const { return orders_.size(); }

private:
    std::vector<Order> orders_;
    std::unordered_set<OrderId> seen_;
    Sequence seq_ = 1;
};

void expectSameReport(const ExecutionReport& got, const ExecutionReport& want, std::size_t step) {
    ASSERT_EQ(got.accepted, want.accepted) << "step " << step;
    ASSERT_EQ(got.reason, want.reason) << "step " << step;
    ASSERT_EQ(got.filled, want.filled) << "step " << step;
    ASSERT_EQ(got.remaining, want.remaining) << "step " << step;
    ASSERT_EQ(got.trades.size(), want.trades.size()) << "step " << step;
    for (std::size_t i = 0; i < got.trades.size(); ++i) {
        ASSERT_EQ(got.trades[i], want.trades[i]) << "step " << step << " trade " << i;
    }
}

void runRandomized(std::uint32_t seed, std::size_t steps) {
    std::mt19937_64 rng(seed);
    OrderBook book;
    ReferenceBook ref;
    std::vector<OrderId> issued;
    OrderId nextId = 1;

    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    for (std::size_t step = 0; step < steps; ++step) {
        int roll = pick(0, 99);
        ExecutionReport got, want;
        if (roll < 60) {
            // Limit order in a narrow band around 100.00 so plenty of orders cross.
            Side side = pick(0, 1) ? Side::Buy : Side::Sell;
            Quantity qty = pick(1, 100);
            Price price = 10000 + pick(-25, 25);
            OrderId id = nextId++;
            // Occasionally reuse an id, or send an invalid quantity or price.
            int odd = pick(0, 199);
            if (odd == 0 && !issued.empty()) id = issued[static_cast<std::size_t>(pick(0, static_cast<int>(issued.size()) - 1))];
            if (odd == 1) qty = pick(-5, 0);
            if (odd == 2) price = pick(-5, 0);
            got = book.addLimit(id, side, qty, price);
            want = ref.add(id, side, OrderType::Limit, qty, price);
            issued.push_back(id);
        } else if (roll < 70) {
            Side side = pick(0, 1) ? Side::Buy : Side::Sell;
            Quantity qty = pick(1, 150);
            OrderId id = nextId++;
            got = book.addMarket(id, side, qty);
            want = ref.add(id, side, OrderType::Market, qty, 0);
            issued.push_back(id);
        } else {
            // Cancel: mostly ids we issued (live, filled, or already cancelled),
            // sometimes ids that never existed.
            OrderId id = (issued.empty() || pick(0, 19) == 0)
                             ? nextId + 1000000
                             : issued[static_cast<std::size_t>(pick(0, static_cast<int>(issued.size()) - 1))];
            got = book.cancel(id);
            want = ref.cancel(id);
        }

        expectSameReport(got, want, step);
        if (::testing::Test::HasFatalFailure()) return;

        std::string why;
        ASSERT_TRUE(book.checkInvariants(&why)) << "step " << step << ": " << why;
        ASSERT_EQ(book.bestBid(), ref.best(Side::Buy)) << "step " << step;
        ASSERT_EQ(book.bestAsk(), ref.best(Side::Sell)) << "step " << step;
        ASSERT_EQ(book.orderCount(), ref.size()) << "step " << step;
    }
}

}  // namespace

TEST(Randomized, MatchesReferenceImplementationOn100kCommands) { runRandomized(42, 100000); }

TEST(Randomized, MatchesReferenceImplementationWithOtherSeeds) {
    for (std::uint32_t seed : {1u, 7u, 2024u}) {
        runRandomized(seed, 20000);
        if (HasFatalFailure()) return;
    }
}
