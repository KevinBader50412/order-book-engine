#pragma once

#include <vector>

#include "orderbook/Trade.hpp"
#include "orderbook/Types.hpp"

namespace ob {

enum class RejectReason {
    None,
    InvalidQuantity,  // quantity <= 0
    InvalidPrice,     // limit price <= 0
    DuplicateId,      // id was already used by an earlier order
    UnknownId,        // cancel: no order with this id was ever seen
    NotActive,        // cancel: the order exists but is already filled or cancelled
};

inline const char* toString(RejectReason r) {
    switch (r) {
        case RejectReason::None: return "none";
        case RejectReason::InvalidQuantity: return "invalid_quantity";
        case RejectReason::InvalidPrice: return "invalid_price";
        case RejectReason::DuplicateId: return "duplicate_id";
        case RejectReason::UnknownId: return "unknown_id";
        case RejectReason::NotActive: return "order_not_active";
    }
    return "unknown";
}

// The result of one operation on the book.
//
//  - addLimit:  filled = quantity traded now, remaining = quantity left resting
//  - addMarket: filled = quantity traded now, remaining = quantity cancelled
//               because the opposite side ran out (market orders never rest)
//  - cancel:    remaining = quantity removed from the book
struct ExecutionReport {
    OrderId id = 0;
    bool accepted = false;
    RejectReason reason = RejectReason::None;
    std::vector<Trade> trades;
    Quantity filled = 0;
    Quantity remaining = 0;

    static ExecutionReport reject(OrderId id, RejectReason r) {
        ExecutionReport rep;
        rep.id = id;
        rep.accepted = false;
        rep.reason = r;
        return rep;
    }
};

}  // namespace ob
