// The engine (note 003): matching over note 002's books. An incoming order
// meets the other side's best level first and, there, the head of its queue
// (4757(a)(1)(A), (B)); each trade is at the resting order's price
// (4757(a)(3)) and reduces both orders by its shares (4757(a)(2)). What is
// left rests, or for an immediate-or-cancel order is cancelled (4703(a)(1)).
// A replace is a new order (ITCH 5.0): it matches if it crosses (D3). So no
// book is crossed after any message.
//
// Each message's reports go to the sink, in order (D2): accepted (the ID) or
// refused (nothing changed); the trades; rested, or cancelled. A cancel
// reports cancelled; a replace reports the old order cancelled, then as a new
// order. Every accepted order takes the next ID, IOC included (D4).
// Design: docs/design/003-matching.md
#pragma once

#include <me/market.hpp>
#include <me/types.hpp>

#include <algorithm>
#include <cstdint>

namespace me {

enum class tif : std::uint8_t { rest, ioc };  // rests until cancelled; immediate or cancel

struct trade_report {
    std::uint64_t match;  // one engine-wide sequence, from 1
    order_id taker, maker;
    instrument_t instrument;
    price_t price;  // the maker's
    shares_t shares, maker_left;
    bool operator==(const trade_report&) const = default;
};

// A sink has: accepted(order_id), refused(refusal), trade(const trade_report&),
// rested(order_id, price_t, shares_t), cancelled(order_id, shares_t).
template <class Book, class Sink>
class engine {
public:
    engine(Book& book, Sink& sink) : book_(book), sink_(sink) {}

    void enter(instrument_t instrument, side s, price_t price, shares_t shares, tif t) {
        if (!valid_size(shares)) return refuse(refusal::size);
        if (!valid_price(price)) return refuse(refusal::price);
        if (instrument >= book_.instruments()) return refuse(refusal::instrument);
        // An order that may rest is refused before it matches if it could not
        // rest (matching frees room on the other side only).
        if (t == tif::rest)
            if (const refusal why = book_.can_rest(instrument, s, price); why != refusal::none) return refuse(why);
        incoming(instrument, s, price, shares, t);
    }

    // Cancel part: the order keeps its place; all it has left removes it.
    void cancel(order_id id, shares_t shares) {
        const std::uint32_t o = book_.at(id);
        if (o == npos) return refuse(refusal::unknown_id);
        if (shares == 0) return refuse(refusal::size);
        if (shares > book_.shares_at(o)) return refuse(refusal::too_many);
        book_.reduce(o, shares);
        sink_.cancelled(id, shares);
    }

    void cancel(order_id id) {
        const std::uint32_t o = book_.at(id);
        if (o == npos) return refuse(refusal::unknown_id);
        const shares_t shares = book_.shares_at(o);
        book_.reduce(o, shares);
        sink_.cancelled(id, shares);
    }

    // A new order in place of the old, same instrument and side, resting
    // until cancelled. If it does not cross, the book's replace (the record
    // reused); if it does, the old one cancelled and the new one entered.
    void replace(order_id id, price_t price, shares_t shares) {
        if (!valid_size(shares)) return refuse(refusal::size);
        if (!valid_price(price)) return refuse(refusal::price);
        const std::uint32_t o = book_.at(id);
        if (o == npos) return refuse(refusal::unknown_id);
        const instrument_t instrument = book_.instrument_at(o);
        const side s = book_.side_at(o);
        const shares_t old = book_.shares_at(o);
        if (!crosses(instrument, s, price)) {
            const answer a = book_.replace(id, price, shares);
            if (!a.ok()) return refuse(a.why);
            sink_.cancelled(id, old);
            sink_.accepted(a.id);
            sink_.rested(a.id, price, shares);
            return;
        }
        if (const refusal why = book_.can_rest(instrument, s, price, true); why != refusal::none) return refuse(why);
        book_.reduce(o, old);
        sink_.cancelled(id, old);
        incoming(instrument, s, price, shares, tif::rest);
    }

    std::uint64_t refused(refusal r) const { return refused_[static_cast<int>(r)]; }
    std::uint64_t matches() const { return match_; }

private:
    void refuse(refusal r) {
        ++refused_[static_cast<int>(r)];
        sink_.refused(r);
    }

    static bool meets(side s, price_t limit, price_t resting) { return s == side::buy ? resting <= limit : resting >= limit; }
    static side other(side s) { return s == side::buy ? side::sell : side::buy; }

    bool crosses(instrument_t instrument, side s, price_t price) const {
        const std::uint32_t h = book_.best_head(instrument, other(s));
        return h != npos && meets(s, price, book_.price_at(h));
    }

    // Checked already: accepted, matched, then rested or cancelled.
    void incoming(instrument_t instrument, side s, price_t price, shares_t shares, tif t) {
        const order_id id = book_.next_id();
        sink_.accepted(id);
        shares_t left = shares;
        const side o_side = other(s);
        while (left) {
            const std::uint32_t h = book_.best_head(instrument, o_side);
            if (h == npos) break;
            const price_t p = book_.price_at(h);
            if (!meets(s, price, p)) break;
            const shares_t q = std::min(left, book_.shares_at(h));
            const order_id maker = book_.id_at(h);
            const shares_t maker_left = book_.reduce(h, q);
            left -= q;
            sink_.trade(trade_report{++match_, id, maker, instrument, p, q, maker_left});
        }
        if (left && t == tif::rest) {
            book_.add(instrument, s, price, left);  // cannot refuse: checked before matching
            sink_.rested(id, price, left);
            return;
        }
        book_.skip_id();
        if (left) sink_.cancelled(id, left);
    }

    Book& book_;
    Sink& sink_;
    std::uint64_t match_ = 0;
    std::uint64_t refused_[refusals] = {};
};

}  // namespace me
