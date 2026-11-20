#include "orderbook/CommandParser.hpp"

#include <cctype>
#include <charconv>
#include <vector>

namespace ob {
namespace {

std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size() || line[i] == '#') break;
        std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])) && line[i] != '#') ++i;
        tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

template <class Int>
bool parseInt(std::string_view s, Int& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    if (*begin == '+') ++begin;
    auto [ptr, ec] = std::from_chars(begin, s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

ParseResult error(std::string msg) {
    ParseResult r;
    r.error = std::move(msg);
    return r;
}

}  // namespace

ParseResult parseCommand(std::string_view line) {
    const auto t = tokenize(line);
    if (t.empty()) return {};  // blank line or comment

    Command cmd;
    if (iequals(t[0], "PRINT")) {
        if (t.size() != 1) return error("PRINT takes no arguments");
        cmd.kind = CommandKind::Print;
        return {cmd, {}};
    }
    if (iequals(t[0], "QUIT") || iequals(t[0], "EXIT")) {
        cmd.kind = CommandKind::Quit;
        return {cmd, {}};
    }
    if (iequals(t[0], "CANCEL")) {
        if (t.size() != 2) return error("usage: CANCEL <id>");
        cmd.kind = CommandKind::Cancel;
        if (!parseInt(t[1], cmd.id)) return error("invalid order id '" + std::string(t[1]) + "'");
        return {cmd, {}};
    }
    if (iequals(t[0], "ADD")) {
        if (t.size() < 5) {
            return error("usage: ADD <id> BUY|SELL LIMIT <qty> <price> | ADD <id> BUY|SELL MARKET <qty>");
        }
        cmd.kind = CommandKind::Add;
        if (!parseInt(t[1], cmd.id)) return error("invalid order id '" + std::string(t[1]) + "'");

        if (iequals(t[2], "BUY")) cmd.side = Side::Buy;
        else if (iequals(t[2], "SELL")) cmd.side = Side::Sell;
        else return error("side must be BUY or SELL, got '" + std::string(t[2]) + "'");

        if (iequals(t[3], "LIMIT")) cmd.type = OrderType::Limit;
        else if (iequals(t[3], "MARKET")) cmd.type = OrderType::Market;
        else return error("type must be LIMIT or MARKET, got '" + std::string(t[3]) + "'");

        // Negative and zero quantities parse fine here and are rejected by the book.
        if (!parseInt(t[4], cmd.quantity)) return error("invalid quantity '" + std::string(t[4]) + "'");

        if (cmd.type == OrderType::Limit) {
            if (t.size() != 6) return error("usage: ADD <id> BUY|SELL LIMIT <qty> <price>");
            auto price = parsePrice(t[5]);
            if (!price) return error("invalid price '" + std::string(t[5]) + "' (at most 2 decimals)");
            cmd.price = *price;
        } else if (t.size() != 5) {
            return error("usage: ADD <id> BUY|SELL MARKET <qty>");
        }
        return {cmd, {}};
    }
    return error("unknown command '" + std::string(t[0]) + "'");
}

}  // namespace ob
