#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace ob {

// Prices are an integer number of ticks. One tick is 0.01, so 101.50 is 10150.
// Floating point is never used for prices: rounding would make equality and
// ordering comparisons unreliable.
using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using Sequence = std::uint64_t;

constexpr Price kTicksPerUnit = 100;
constexpr int kPriceDecimals = 2;

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };

inline const char* toString(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }
inline const char* toString(OrderType t) { return t == OrderType::Limit ? "LIMIT" : "MARKET"; }

// Parses a decimal string such as "101.5" or "101.50" straight into ticks,
// without going through a double. At most two decimal places are accepted.
inline std::optional<Price> parsePrice(std::string_view s) {
    if (s.empty()) return std::nullopt;
    bool negative = false;
    std::size_t i = 0;
    if (s[0] == '-' || s[0] == '+') {
        negative = s[0] == '-';
        i = 1;
    }
    constexpr Price kMax = std::numeric_limits<Price>::max() / 10;
    Price whole = 0;
    bool sawDigit = false;
    for (; i < s.size() && s[i] != '.'; ++i) {
        char c = s[i];
        if (c < '0' || c > '9') return std::nullopt;
        if (whole > kMax / kTicksPerUnit) return std::nullopt;  // would overflow
        whole = whole * 10 + (c - '0');
        sawDigit = true;
    }
    Price frac = 0;
    int decimals = 0;
    if (i < s.size()) {  // s[i] == '.'
        ++i;
        for (; i < s.size(); ++i) {
            char c = s[i];
            if (c < '0' || c > '9') return std::nullopt;
            if (++decimals > kPriceDecimals) return std::nullopt;  // finer than one tick
            frac = frac * 10 + (c - '0');
            sawDigit = true;
        }
    }
    if (!sawDigit) return std::nullopt;
    for (int d = decimals; d < kPriceDecimals; ++d) frac *= 10;
    Price ticks = whole * kTicksPerUnit + frac;
    return negative ? -ticks : ticks;
}

inline std::string formatPrice(Price ticks) {
    std::string sign = ticks < 0 ? "-" : "";
    std::uint64_t abs = ticks < 0 ? static_cast<std::uint64_t>(-(ticks + 1)) + 1
                                  : static_cast<std::uint64_t>(ticks);
    std::uint64_t whole = abs / kTicksPerUnit;
    std::uint64_t frac = abs % kTicksPerUnit;
    std::string f = std::to_string(frac);
    if (f.size() < 2) f.insert(0, "0");
    return sign + std::to_string(whole) + "." + f;
}

}  // namespace ob
