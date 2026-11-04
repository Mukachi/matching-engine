// The reference engine (note 003): the same rules and reports as `engine`,
// written to be read, on the reference books (ordered maps of lists). The
// oracle for the engine and its baseline. No capacity: it never refuses for
// lack of room.
// Design: docs/design/003-matching.md
#pragma once

#include <me/engine.hpp>
#include <me/reference_market.hpp>

#include <algorithm>
#include <cstdint>

namespace me {

template <class Sink>
class reference_engine {
public:
    reference_engine(reference_market& book, Sink& sink) : book_(book), sink_(sink) {}

    void enter(instrument_t instrument, side s, price_t price, shares_t shares, tif t) {
        if (!valid_size(shares)) return refuse(refusal::size);
        if (!valid_price(price)) return refuse(refusal::price);
        if (instrument >= book_.instruments()) return refuse(refusal::instrument);
        incoming(instrument, s, price, shares, t);
    }

    void cancel(order_id id, shares_t shares) {
        const answer a = book_.cancel(id, shares);
        if (!a.ok()) return refuse(a.why);
        sink_.cancelled(id, shares);
    }

    void cancel(order_id id) {
        const auto o = book_.find(id);
        if (!o) return refuse(refusal::unknown_id);
        book_.cancel(id);
        sink_.cancelled(id, o->shares);
    }

    void replace(order_id id, price_t price, shares_t shares) {
        if (!valid_size(shares)) return refuse(refusal::size);
        if (!valid_price(price)) return refuse(refusal::price);
        const auto o = book_.find(id);
        if (!o) return refuse(refusal::unknown_id);
        book_.cancel(id);
        sink_.cancelled(id, o->shares);
        incoming(o->instrument, o->side, price, shares, tif::rest);
    }

    std::uint64_t refused(refusal r) const { return refused_[static_cast<int>(r)]; }

private:
    void refuse(refusal r) {
        ++refused_[static_cast<int>(r)];
        sink_.refused(r);
    }

    // Against the other side from its best price, each level from its front,
    // while the price meets the limit; then rest what is left, or cancel it.
    void incoming(instrument_t instrument, side s, price_t limit, shares_t shares, tif t) {
        const order_id id = book_.next_id();
        sink_.accepted(id);
        const side against = s == side::buy ? side::sell : side::buy;
        while (shares > 0) {
            const auto best = book_.best(instrument, against);
            if (!best) break;
            const auto& [price, maker] = *best;
            const bool meets = s == side::buy ? price <= limit : price >= limit;
            if (!meets) break;
            const shares_t q = std::min(shares, maker.shares);
            book_.cancel(maker.id, q);
            shares -= q;
            sink_.trade(trade_report{++match_, id, maker.id, instrument, price, q, maker.shares - q});
        }
        if (shares > 0 && t == tif::rest) {
            book_.add(instrument, s, limit, shares);
            sink_.rested(id, limit, shares);
            return;
        }
        book_.skip_id();
        if (shares > 0) sink_.cancelled(id, shares);
    }

    reference_market& book_;
    Sink& sink_;
    std::uint64_t match_ = 0;
    std::uint64_t refused_[refusals] = {};
};

}  // namespace me
