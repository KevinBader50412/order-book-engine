#pragma once

#include "orderbook/Types.hpp"

namespace ob {

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Quantity quantity = 0;  // remaining (unfilled) quantity
    Price price = 0;        // only meaningful for limit orders
    Sequence sequence = 0;  // arrival order, assigned by the book
};

}  // namespace ob
