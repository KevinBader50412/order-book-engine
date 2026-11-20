#include "orderbook/OrderBook.hpp"

#include <algorithm>
#include <iterator>
#include <sstream>

namespace ob {

ExecutionReport OrderBook::validateNew(OrderId id, Quantity quantity) const {
    if (quantity <= 0) return ExecutionReport::reject(id, RejectReason::InvalidQuantity);
    if (seenIds_.count(id) != 0) return ExecutionReport::reject(id, RejectReason::DuplicateId);
    ExecutionReport ok;
    ok.id = id;
    ok.accepted = true;
    return ok;
}

// Matches `incoming` against the opposite side, best level first and oldest
// order first within a level, while `crosses(levelPrice)` holds.
template <class Levels, class Crosses>
void OrderBook::match(Levels& opposite, Order& incoming, Crosses crosses, ExecutionReport& report) {
    while (incoming.quantity > 0 && !opposite.empty()) {
        auto levelIt = opposite.begin();
        if (!crosses(levelIt->first)) break;
        PriceLevel& level = levelIt->second;

        while (incoming.quantity > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();
            const Quantity qty = std::min(incoming.quantity, resting.quantity);

            Trade trade;
            trade.buyId = incoming.side == Side::Buy ? incoming.id : resting.id;
            trade.sellId = incoming.side == Side::Buy ? resting.id : incoming.id;
            trade.price = resting.price;  // always the resting order's price
            trade.quantity = qty;
            report.trades.push_back(trade);

            incoming.quantity -= qty;
            resting.quantity -= qty;
            level.total -= qty;
            report.filled += qty;

            if (resting.quantity == 0) {
                index_.erase(resting.id);
                level.orders.pop_front();
            }
        }
        if (level.orders.empty()) opposite.erase(levelIt);
    }
}

template <class Levels>
typename Levels::iterator OrderBook::rest(Levels& levels, const Order& order) {
    auto [levelIt, inserted] = levels.try_emplace(order.price);
    (void)inserted;
    PriceLevel& level = levelIt->second;
    level.orders.push_back(order);
    level.total += order.quantity;

    Locator loc;
    loc.side = order.side;
    loc.order = std::prev(level.orders.end());
    if constexpr (std::is_same_v<Levels, BidLevels>) {
        loc.bidLevel = levelIt;
    } else {
        loc.askLevel = levelIt;
    }
    index_.emplace(order.id, loc);
    return levelIt;
}

template <class Levels>
Quantity OrderBook::removeResting(Levels& levels, typename Levels::iterator level,
                                  std::list<Order>::iterator order) {
    const Quantity qty = order->quantity;
    level->second.total -= qty;
    level->second.orders.erase(order);
    if (level->second.orders.empty()) levels.erase(level);
    return qty;
}

ExecutionReport OrderBook::addLimit(OrderId id, Side side, Quantity quantity, Price price) {
    ExecutionReport report = validateNew(id, quantity);
    if (!report.accepted) return report;
    if (price <= 0) return ExecutionReport::reject(id, RejectReason::InvalidPrice);

    seenIds_.insert(id);
    Order order{id, side, OrderType::Limit, quantity, price, nextSequence_++};

    if (side == Side::Buy) {
        // Rule 1: a buy at P matches asks priced at or below P.
        match(asks_, order, [price](Price ask) { return ask <= price; }, report);
        if (order.quantity > 0) rest(bids_, order);
    } else {
        // Rule 2: a sell at P matches bids priced at or above P.
        match(bids_, order, [price](Price bid) { return bid >= price; }, report);
        if (order.quantity > 0) rest(asks_, order);
    }
    report.remaining = order.quantity;
    return report;
}

ExecutionReport OrderBook::addMarket(OrderId id, Side side, Quantity quantity) {
    ExecutionReport report = validateNew(id, quantity);
    if (!report.accepted) return report;

    seenIds_.insert(id);
    Order order{id, side, OrderType::Market, quantity, 0, nextSequence_++};

    // Rule 3: match at any price; whatever is left is cancelled, never rested.
    auto any = [](Price) { return true; };
    if (side == Side::Buy) {
        match(asks_, order, any, report);
    } else {
        match(bids_, order, any, report);
    }
    report.remaining = order.quantity;
    return report;
}

ExecutionReport OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        // Rule 4: distinguish "never existed" from "already filled or cancelled".
        return ExecutionReport::reject(
            id, seenIds_.count(id) ? RejectReason::NotActive : RejectReason::UnknownId);
    }
    const Locator loc = it->second;
    index_.erase(it);

    ExecutionReport report;
    report.id = id;
    report.accepted = true;
    report.remaining = loc.side == Side::Buy ? removeResting(bids_, loc.bidLevel, loc.order)
                                             : removeResting(asks_, loc.askLevel, loc.order);
    return report;
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

OrderBook::Depth OrderBook::depth(std::size_t levels) const {
    Depth d;
    for (auto it = bids_.begin(); it != bids_.end() && d.bids.size() < levels; ++it) {
        d.bids.push_back({it->first, it->second.total, it->second.orders.size()});
    }
    for (auto it = asks_.begin(); it != asks_.end() && d.asks.size() < levels; ++it) {
        d.asks.push_back({it->first, it->second.total, it->second.orders.size()});
    }
    return d;
}

std::optional<Quantity> OrderBook::restingQuantity(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return it->second.order->quantity;
}

Quantity OrderBook::totalRestingQuantity() const {
    Quantity total = 0;
    for (const auto& [price, level] : bids_) total += level.total;
    for (const auto& [price, level] : asks_) total += level.total;
    return total;
}

namespace {
bool fail(std::string* why, const std::string& msg) {
    if (why) *why = msg;
    return false;
}
}  // namespace

template <class Levels>
bool OrderBook::checkSide(const Levels& levels, Side side, std::size_t& orders,
                          std::string* why) const {
    for (const auto& [price, level] : levels) {
        std::ostringstream where;
        where << toString(side) << " level " << formatPrice(price) << ": ";
        if (level.orders.empty()) return fail(why, where.str() + "empty level left in book");
        Quantity sum = 0;
        Sequence lastSeq = 0;
        for (const Order& o : level.orders) {
            if (o.quantity <= 0) return fail(why, where.str() + "order with non-positive quantity");
            if (o.price != price) return fail(why, where.str() + "order price differs from level");
            if (o.side != side) return fail(why, where.str() + "order on the wrong side");
            if (o.sequence <= lastSeq) return fail(why, where.str() + "orders not in arrival order");
            lastSeq = o.sequence;
            sum += o.quantity;

            auto idx = index_.find(o.id);
            if (idx == index_.end()) return fail(why, where.str() + "resting order missing from index");
            if (&*idx->second.order != &o) return fail(why, where.str() + "index points elsewhere");
            ++orders;
        }
        if (sum != level.total) return fail(why, where.str() + "level total does not match orders");
    }
    return true;
}

bool OrderBook::checkInvariants(std::string* why) const {
    // Rule 6: the book is never crossed.
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        return fail(why, "book is crossed: best bid " + formatPrice(bids_.begin()->first) +
                             " >= best ask " + formatPrice(asks_.begin()->first));
    }
    std::size_t orders = 0;
    if (!checkSide(bids_, Side::Buy, orders, why)) return false;
    if (!checkSide(asks_, Side::Sell, orders, why)) return false;
    if (orders != index_.size()) return fail(why, "index size does not match resting orders");
    return true;
}

}  // namespace ob
