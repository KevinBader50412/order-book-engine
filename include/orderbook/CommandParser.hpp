#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "orderbook/Types.hpp"

namespace ob {

enum class CommandKind { Add, Cancel, Print, Quit };

struct Command {
    CommandKind kind = CommandKind::Add;
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Quantity quantity = 0;
    Price price = 0;
};

struct ParseResult {
    std::optional<Command> command;  // empty for blank lines, comments and errors
    std::string error;               // non-empty only when the line is invalid

    bool ok() const { return error.empty(); }
};

// Grammar (keywords are case-insensitive, '#' starts a comment):
//   ADD <id> BUY|SELL LIMIT <qty> <price>
//   ADD <id> BUY|SELL MARKET <qty>
//   CANCEL <id>
//   PRINT
//   QUIT
//
// The parser only checks syntax. Business rules (quantity > 0, price > 0,
// unique ids) are enforced by the order book so they apply to every caller.
ParseResult parseCommand(std::string_view line);

}  // namespace ob
