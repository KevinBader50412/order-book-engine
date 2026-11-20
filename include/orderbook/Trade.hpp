#pragma once

#include <ostream>

#include "orderbook/Types.hpp"

namespace ob {

// A trade always executes at the resting order's price.
struct Trade {
    OrderId buyId = 0;
    OrderId sellId = 0;
    Price price = 0;
    Quantity quantity = 0;

    bool operator==(const Trade& o) const {
        return buyId == o.buyId && sellId == o.sellId && price == o.price && quantity == o.quantity;
    }
    bool operator!=(const Trade& o) const { return !(*this == o); }
};

inline std::ostream& operator<<(std::ostream& os, const Trade& t) {
    return os << "TRADE buy=" << t.buyId << " sell=" << t.sellId << " price=" << formatPrice(t.price)
              << " qty=" << t.quantity;
}

}  // namespace ob
