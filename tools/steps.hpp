// The sample day as note 004's steps, decided once before anything is timed
// (note 005): the replay on the default book through its engine, each step
// kept, so a benchmark gives every engine the same steps and no timed window
// holds decoding or the reference map. The conversion's end state is kept for
// the benchmarks to check each engine against.
// Design: docs/design/005-measurement.md
#pragma once

#include "day_file.hpp"
#include "replay.hpp"

#include <me/engine.hpp>
#include <me/market.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace me {

static_assert(sizeof(step) == 24);

// A sink that counts what an engine's checks need: trades, and anything refused or without room.
struct tally {
    std::uint64_t trades = 0, refused_any = 0, no_room = 0;
    void accepted(order_id) {}
    void refused(refusal) { ++refused_any; }
    void trade(const trade_report&) { ++trades; }
    void rested(order_id, price_t, shares_t) {}
    void cancelled(order_id, shares_t, cause c) { no_room += c == cause::no_room; }
};

// What an engine should hold after the steps.
struct end_state {
    std::size_t live_orders = 0;
    order_id next_id = 0;
    std::uint64_t trades = 0;
    bool operator==(const end_state&) const = default;
};

struct day_steps {
    std::vector<step> steps;
    std::uint64_t messages = 0;  // read to make them
    end_state end;
    bool clean = false;  // read, nothing refused, nothing without room, no unknown reference
};

// Up to `max_steps` steps of the day at `path`.
inline day_steps convert_day(const std::string& path, std::size_t max_steps = ~std::size_t{0}) {
    day_steps d;
    day_file f(path);
    if (!f.open()) return d;
    d.steps.reserve(std::min<std::size_t>(max_steps, 420'000'000));
    auto b = std::make_unique<book>(config{.prefault = false});
    tally k;
    engine<book, tally> e(*b, k);
    replay p;
    itchfh::message m;
    while (d.steps.size() < max_steps && f.next(m)) {
        const step st = p.next(m, *b);
        if (st.k == step::none) continue;
        apply(st, e, *b);
        p.done();
        d.steps.push_back(st);
    }
    d.messages = f.sequence();
    d.end = {b->live_orders(), b->next_id(), k.trades};
    d.clean = d.messages > 0 && k.refused_any == 0 && k.no_room == 0 && p.counts().unknown == 0;
    return d;
}

}  // namespace me
