#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "orderbook/ExecutionReport.hpp"
#include "orderbook/Order.hpp"
#include "orderbook/Types.hpp"

namespace ob {

// A single-symbol limit order book with price-time priority.
//
// Not thread-safe by design: exactly one thread (the matching thread) owns a
// book. See MatchingEngine for how orders from many threads reach it.
class OrderBook {
public:
    struct LevelInfo {
        Price price = 0;
        Quantity quantity = 0;
        std::size_t orders = 0;
    };
    struct Depth {
        std::vector<LevelInfo> bids;  // best (highest) first
        std::vector<LevelInfo> asks;  // best (lowest) first
    };

    ExecutionReport addLimit(OrderId id, Side side, Quantity quantity, Price price);
    ExecutionReport addMarket(OrderId id, Side side, Quantity quantity);
    ExecutionReport cancel(OrderId id);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Depth depth(std::size_t levels) const;

    // Quantity still resting for this order, or nullopt if it is not in the book.
    std::optional<Quantity> restingQuantity(OrderId id) const;

    std::size_t orderCount() const { return index_.size(); }
    std::size_t bidLevels() const { return bids_.size(); }
    std::size_t askLevels() const { return asks_.size(); }
    Quantity totalRestingQuantity() const;

    // Verifies every structural invariant of the book. Intended for tests.
    // On failure returns false and, if `why` is given, describes the problem.
    bool checkInvariants(std::string* why = nullptr) const;

private:
    struct PriceLevel {
        std::list<Order> orders;  // arrival order: front() matches first
        Quantity total = 0;       // sum of remaining quantity at this level
    };
    using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;  // begin() = best bid
    using AskLevels = std::map<Price, PriceLevel>;                       // begin() = best ask

    // Where a resting order lives. std::map and std::list iterators stay
    // valid when other elements are inserted or erased, so a cancel can go
    // straight to the order without searching.
    struct Locator {
        Side side = Side::Buy;
        BidLevels::iterator bidLevel;
        AskLevels::iterator askLevel;
        std::list<Order>::iterator order;
    };

    ExecutionReport validateNew(OrderId id, Quantity quantity) const;

    template <class Levels, class Crosses>
    void match(Levels& opposite, Order& incoming, Crosses crosses, ExecutionReport& report);

    template <class Levels>
    typename Levels::iterator rest(Levels& levels, const Order& order);

    template <class Levels>
    Quantity removeResting(Levels& levels, typename Levels::iterator level,
                           std::list<Order>::iterator order);

    template <class Levels>
    bool checkSide(const Levels& levels, Side side, std::size_t& orders, std::string* why) const;

    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, Locator> index_;  // resting orders only
    std::unordered_set<OrderId> seenIds_;         // every accepted id, for duplicate detection
    Sequence nextSequence_ = 1;
};

}  // namespace ob
