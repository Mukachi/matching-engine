// A sink that records every report (note 003, D2), for the tests: each
// message's reports compared in sequence.
#pragma once

#include <me/engine.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace me::test {

struct report {
    enum kind { accepted, refused, trade, rested, cancelled } k;
    order_id id = 0;  // accepted, rested, cancelled
    refusal why = refusal::none;
    trade_report t{};
    price_t price = 0;
    shares_t shares = 0;
    bool operator==(const report&) const = default;
};

inline std::string to_string(const report& r) {
    char b[160];
    switch (r.k) {
        case report::accepted: std::snprintf(b, sizeof b, "accepted %llu", (unsigned long long)r.id); break;
        case report::refused: std::snprintf(b, sizeof b, "refused %s", me::to_string(r.why)); break;
        case report::trade:
            std::snprintf(b, sizeof b, "trade #%llu taker %llu maker %llu @%u x%u left %u", (unsigned long long)r.t.match,
                          (unsigned long long)r.t.taker, (unsigned long long)r.t.maker, r.t.price, r.t.shares, r.t.maker_left);
            break;
        case report::rested: std::snprintf(b, sizeof b, "rested %llu @%u x%u", (unsigned long long)r.id, r.price, r.shares); break;
        case report::cancelled: std::snprintf(b, sizeof b, "cancelled %llu x%u", (unsigned long long)r.id, r.shares); break;
    }
    return b;
}

struct recorder {
    std::vector<report> out;
    void accepted(order_id id) { out.push_back({report::accepted, id}); }
    void refused(refusal why) { out.push_back({report::refused, 0, why}); }
    void trade(const trade_report& t) { out.push_back({report::trade, 0, refusal::none, t}); }
    void rested(order_id id, price_t p, shares_t s) { out.push_back({report::rested, id, refusal::none, {}, p, s}); }
    void cancelled(order_id id, shares_t s) { out.push_back({report::cancelled, id, refusal::none, {}, 0, s}); }
    // This message's reports, taken.
    std::vector<report> take() { return std::exchange(out, {}); }
};

}  // namespace me::test
