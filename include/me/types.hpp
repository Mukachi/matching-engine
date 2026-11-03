// The book's types and the rules' limits (note 002): prices with four implied
// decimals, as ITCH 5.0; sizes and prices as Rule 4703 allows them; every
// operation answered, a refusal with its reason (D6).
// Design: docs/design/002-book.md
#pragma once

#include <cstdint>

namespace me {

using order_id = std::uint64_t;  // the engine's, in sequence from 1
using price_t = std::uint32_t;   // four implied decimals
using shares_t = std::uint32_t;
using instrument_t = std::uint16_t;

enum class side : std::uint8_t { buy, sell };

inline constexpr shares_t max_shares = 999'999;      // 4703(b): whole shares, 1 to 999,999
inline constexpr price_t max_price = 1'999'999'900;  // 4703(c): at most $199,999.99
inline constexpr std::uint32_t npos = 0xFFFFFFFFu;

enum class refusal : std::uint8_t {
    none,
    size,           // 4703(b)
    price,          // 4703(c)
    instrument,     // beyond the configured instruments
    unknown_id,     // never entered, or already finished
    too_many,       // a cancel of more shares than remain
    no_orders,      // D4: the orders live at once
    no_levels,      // D4: the levels live at once
    no_ids,         // D4: D1 A's IDs entered
};
inline constexpr int refusals = 9;

inline const char* to_string(refusal r) {
    switch (r) {
        case refusal::none: return "none";
        case refusal::size: return "size";
        case refusal::price: return "price";
        case refusal::instrument: return "instrument";
        case refusal::unknown_id: return "unknown id";
        case refusal::too_many: return "too many shares";
        case refusal::no_orders: return "no order capacity";
        case refusal::no_levels: return "no level capacity";
        case refusal::no_ids: return "no id capacity";
    }
    return "?";
}

// An operation's answer: refused (and why), or done, with the order's ID for an
// add or a replace (a replace's new order, note 001's D2).
struct answer {
    refusal why = refusal::none;
    order_id id = 0;
    bool ok() const { return why == refusal::none; }
};

// D4: capacities fixed at start. Defaults from the sample day (the feed
// handler's note 002: 1,925,638 orders live at its peak; Stock Locate codes up
// to 8,915 of a 16-bit field), with headroom.
struct config {
    std::size_t orders = 4'000'000;            // live at once
    std::size_t levels = 2'000'000;            // live at once, all books together
    std::size_t level_arena = std::size_t{1} << 24;  // D2 A: entries of the sides' arrays
    std::size_t ids = std::size_t{1} << 28;    // D1 A: IDs entered (the day: 223 million adds and replaces)
    std::size_t instruments = 65'536;
    bool prefault = true;                      // off: pages faulted in as used (tests, small machines)
};

}  // namespace me
