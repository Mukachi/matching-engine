// The sample day replayed as order entry (note 004): each displayed book event
// turned into one step for the engines, with the day's reference numbers
// mapped to the engine's IDs here, outside the engines. Before each step the
// replay asks a book what entering it would do (D1): when the engine would do
// what Nasdaq did, the step enters it (an execution as an IOC on the other
// side at the order's price, for the shares executed); when not, the step
// applies Nasdaq's outcome to the book directly (an execution as a cancel of
// part, an add or a replace resting without matching), and the message is
// counted by its cause (D2). So the books stay Nasdaq's after every message.
// Design: docs/design/004-replay.md
#pragma once

#include <me/engine.hpp>
#include <me/market.hpp>
#include <me/reference_market.hpp>
#include <me/types.hpp>

#include <itchfh/messages.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace me {

// The first order a side shows an incoming order: its price, ID and shares.
struct head {
    price_t price;
    order_id id;
    shares_t shares;
};
inline std::optional<head> head_of(const reference_market& m, instrument_t i, side s) {
    const auto b = m.best(i, s);
    if (!b) return std::nullopt;
    return head{b->first, b->second.id, b->second.shares};
}
template <class L, class T>
std::optional<head> head_of(const market<L, T>& m, instrument_t i, side s) {
    const std::uint32_t h = m.best_head(i, s);
    if (h == npos) return std::nullopt;
    return head{m.price_at(h), m.id_at(h), m.shares_at(h)};
}

// D2's causes, in the order a miss is put in the first that fits.
enum class miss : std::uint8_t { cross, not_trading, better_price, earlier_orders, unexplained };
constexpr int misses = 5;
inline const char* to_string(miss m) {
    constexpr const char* n[] = {"cross", "not trading", "better price ahead", "earlier orders ahead", "unexplained"};
    return n[static_cast<int>(m)];
}

struct step {
    enum kind : std::uint8_t {
        none,           // not a book event
        enter,          // an add, through matching (rests: it does not cross)
        rest,           // an add that would cross, resting without matching
        cancel_part,    // X
        cancel_all,     // D
        replace,        // U, through the engine's replace (it does not cross)
        replace_rest,   // U that would cross: the book's replace, without matching
        execute,        // E or C reproduced: an IOC on the other side
        execute_apply,  // E or C not reproduced: a cancel of part of the executed order
    } k = none;
    instrument_t instrument = 0;
    me::side s = side::buy;  // enter, rest: the order's; execute: the IOC's (the other side)
    price_t price = 0;
    shares_t shares = 0;
    order_id id = 0;  // the order named (execute: the maker expected)
};

struct replay_counts {
    std::uint64_t executions = 0, reproduced = 0, by_cause[misses] = {};
    std::uint64_t c_other_price = 0, c_other_price_reproduced = 0;  // printable C at a price not its order's
    std::uint64_t c_other_price_by_cause[misses] = {};               // of those not reproduced, by cause
    std::uint64_t adds = 0, adds_rested[misses] = {}, replaces = 0, replaces_rested[misses] = {};
    std::uint64_t unknown = 0;  // a book event for a reference never added
};

class replay {
public:
    // The step for message `m`, decided against `book` (the engine's book
    // before the step); `none` for anything else. Call `done` after the step
    // has been given to every engine.
    template <class Book>
    step next(const itchfh::message& m, const Book& book) {
        step st;
        pending_ = {};
        switch (m.type) {
            case 'H':
                if (m.locate < state_.size()) state_[m.locate] = m.H.trading_state;
                return st;
            case 'A':
            case 'F': {
                const side s = m.A.side == 'B' ? side::buy : side::sell;
                ++c_.adds;
                st = {crosses(book, m.locate, s, m.A.price) ? step::rest : step::enter, m.locate, s, m.A.price, m.A.shares, 0};
                if (st.k == step::rest) ++c_.adds_rested[static_cast<int>(rest_cause(m.locate))];
                pending_ = {m.A.ref, book.next_id(), 0};
                return st;
            }
            case 'X': {
                const auto o = order(book, m.X.ref);
                if (!o) return unknown();
                st = {step::cancel_part, o->instrument, o->side, o->price, m.X.shares, o->id};
                if (m.X.shares >= o->shares) pending_ = {0, 0, m.X.ref};
                return st;
            }
            case 'D': {
                const auto o = order(book, m.D.ref);
                if (!o) return unknown();
                pending_ = {0, 0, m.D.ref};
                return {step::cancel_all, o->instrument, o->side, o->price, o->shares, o->id};
            }
            case 'U': {
                const auto o = order(book, m.U.old_ref);
                if (!o) return unknown();
                ++c_.replaces;
                const bool cross = crosses(book, o->instrument, o->side, m.U.price);
                if (cross) ++c_.replaces_rested[static_cast<int>(rest_cause(o->instrument))];
                pending_ = {m.U.new_ref, book.next_id(), m.U.old_ref};
                return {cross ? step::replace_rest : step::replace, o->instrument, o->side, m.U.price, m.U.shares, o->id};
            }
            case 'E':
            case 'C': {
                const auto o = order(book, m.E.ref);
                if (!o) return unknown();
                ++c_.executions;
                const shares_t q = m.E.shares;
                const side taker = o->side == side::buy ? side::sell : side::buy;
                const auto h = head_of(book, o->instrument, o->side);
                const bool same = h && h->id == o->id;
                const bool other_price = m.type == 'C' && m.E.printable == 'Y' && m.E.price != o->price;
                c_.c_other_price += other_price;
                if (q >= o->shares) pending_ = {0, 0, m.E.ref};
                if (same) {
                    ++c_.reproduced;
                    c_.c_other_price_reproduced += other_price;
                    return {step::execute, o->instrument, taker, o->price, q, o->id};
                }
                miss why = miss::unexplained;
                if (m.type == 'C' && m.E.printable == 'N') why = miss::cross;
                else if (!trading(o->instrument)) why = miss::not_trading;
                else if (h && h->price != o->price) why = miss::better_price;
                else if (h) why = miss::earlier_orders;
                ++c_.by_cause[static_cast<int>(why)];
                c_.c_other_price_by_cause[static_cast<int>(why)] += other_price;
                return {step::execute_apply, o->instrument, o->side, o->price, q, o->id};
            }
            default:
                return st;
        }
    }

    // After the step: the new reference mapped, a finished one dropped.
    void done() {
        if (pending_.drop) id_of_.erase(pending_.drop);
        if (pending_.ref) id_of_[pending_.ref] = pending_.id;
    }

    const replay_counts& counts() const { return c_; }
    // The engine's ID for a reference number still live (0 if none): for comparing with the feed handler's book.
    order_id id_of(std::uint64_t ref) const {
        const auto it = id_of_.find(ref);
        return it == id_of_.end() ? 0 : it->second;
    }

private:
    struct pending {
        std::uint64_t ref;  // a reference to map
        order_id id;        // to this ID
        std::uint64_t drop; // a reference finished
    };

    template <class Book>
    std::optional<order_view> order(const Book& book, std::uint64_t ref) const {
        const auto it = id_of_.find(ref);
        if (it == id_of_.end()) return std::nullopt;
        return book.find(it->second);
    }
    template <class Book>
    static bool crosses(const Book& book, instrument_t i, side s, price_t p) {
        const auto h = head_of(book, i, s == side::buy ? side::sell : side::buy);
        return h && (s == side::buy ? h->price <= p : h->price >= p);
    }
    step unknown() {
        ++c_.unknown;
        return {};
    }
    bool trading(instrument_t i) const { return i >= state_.size() || state_[i] == 'T'; }
    miss rest_cause(instrument_t i) const { return trading(i) ? miss::unexplained : miss::not_trading; }

    std::unordered_map<std::uint64_t, order_id> id_of_;
    std::vector<char> state_ = std::vector<char>(65'536, 'T');
    replay_counts c_;
    pending pending_{};
};

// A step given to an engine (and, for the steps that bypass matching, its book).
template <class Engine, class Book>
void apply(const step& st, Engine& e, Book& book) {
    switch (st.k) {
        case step::none: break;
        case step::enter: e.enter(st.instrument, st.s, st.price, st.shares, tif::rest); break;
        case step::rest: book.add(st.instrument, st.s, st.price, st.shares); break;
        case step::cancel_part: e.cancel(st.id, st.shares); break;
        case step::cancel_all: e.cancel(st.id); break;
        case step::replace: e.replace(st.id, st.price, st.shares); break;
        case step::replace_rest: book.replace(st.id, st.price, st.shares); break;
        case step::execute: e.enter(st.instrument, st.s, st.price, st.shares, tif::ioc); break;
        case step::execute_apply: e.cancel(st.id, st.shares); break;
    }
}

}  // namespace me
