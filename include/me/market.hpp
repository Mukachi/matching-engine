// The books (note 002): one per instrument, each side's levels by price (D2),
// each level's queue in time priority, an intrusive doubly linked list through
// the order records (D3), the records and levels from pools fixed at start
// (D4), an order found by its ID (D1). Add, cancel part or whole, replace,
// each answered; a refusal changes nothing (D6). The book rests what it is
// given: keeping it uncrossed is the engine's (D5, note 003).
//
// The rules (note 001, D2): price, then time among displayed orders
// (4757(a)(1)(A), (B)); a cancel at any time (4703(a)); cancelling part keeps
// the order's place, a replace is a new order at the back of its level,
// whatever changed (ITCH 5.0, Order Cancel and Order Replace); sizes and
// prices as 4703(b) and (c) allow.
// Design: docs/design/002-book.md
#pragma once

#include <me/levels.hpp>
#include <me/memory.hpp>
#include <me/order_table.hpp>
#include <me/types.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace me {

// What tests and tools see of an order and of a level.
struct order_view {
    order_id id;
    instrument_t instrument;
    me::side side;
    price_t price;
    shares_t shares;
};
struct queued {
    order_id id;
    shares_t shares;
    bool operator==(const queued&) const = default;
};
struct level_state {
    price_t price;
    std::uint64_t shares;
    std::uint32_t count;
    std::vector<queued> queue;  // front first
    bool operator==(const level_state&) const = default;
};

inline bool valid_size(shares_t s) { return s >= 1 && s <= max_shares; }
inline bool valid_price(price_t p) { return p >= 1 && p <= max_price; }

template <class Levels, class Table>
class market {
public:
    explicit market(const config& c = {})
        : cfg_(c), shared_(c), table_(c), orders_(zeroed<order_rec>(c.orders, c.prefault)), levels_(zeroed<level_rec>(c.levels, c.prefault)) {
        // Free lists through the pools: order records by `next`, levels by `first`.
        for (std::size_t i = 0; i < c.orders; ++i) orders_[i].next = i + 1 < c.orders ? static_cast<std::uint32_t>(i + 1) : npos;
        for (std::size_t i = 0; i < c.levels; ++i) levels_[i].first = i + 1 < c.levels ? static_cast<std::uint32_t>(i + 1) : npos;
        free_order_ = c.orders ? 0 : npos;
        free_level_ = c.levels ? 0 : npos;
        books_.reserve(c.instruments);
        for (std::size_t i = 0; i < c.instruments; ++i) books_.emplace_back(shared_);
    }

    // A new order, resting at the back of its level.
    answer add(instrument_t instrument, side s, price_t price, shares_t shares) {
        if (!valid_size(shares)) return refuse(refusal::size);
        if (!valid_price(price)) return refuse(refusal::price);
        if (instrument >= books_.size()) return refuse(refusal::instrument);
        const order_id id = next_id_;
        if (!table_.can_take(id)) return refuse(refusal::no_ids);
        if (free_order_ == npos) return refuse(refusal::no_orders);
        const std::uint32_t l = level_for(instrument, s, price);
        if (l == npos) return refuse(refusal::no_levels);
        const std::uint32_t o = free_order_;
        free_order_ = orders_[o].next;
        orders_[o] = {id, price, shares, l, npos, npos, instrument, s};
        append(o, l);
        table_.put(id, o);
        ++next_id_;
        if (++live_orders_ > peak_orders_) peak_orders_ = live_orders_;
        return {refusal::none, id};
    }

    // Cancel part: the order keeps its place; cancelling all it has left
    // removes it. More than it has is refused.
    answer cancel(order_id id, shares_t shares) {
        const std::uint32_t o = table_.get(id);
        if (o == npos) return refuse(refusal::unknown_id);
        if (shares == 0) return refuse(refusal::size);
        order_rec& r = orders_[o];
        if (shares > r.shares) return refuse(refusal::too_many);
        if (shares == r.shares) {
            remove(o);
            return {refusal::none, id};
        }
        r.shares -= shares;
        levels_[r.level].shares -= shares;
        return {refusal::none, id};
    }

    // Cancel whole.
    answer cancel(order_id id) {
        const std::uint32_t o = table_.get(id);
        if (o == npos) return refuse(refusal::unknown_id);
        remove(o);
        return {refusal::none, id};
    }

    // A new order in place of the old: a new ID, at the back of its level,
    // same instrument and side. Everything that can refuse is checked before
    // anything changes; a replace needing a new level is refused at level
    // capacity even when its old level would be freed.
    answer replace(order_id id, price_t price, shares_t shares) {
        if (!valid_size(shares)) return refuse(refusal::size);
        if (!valid_price(price)) return refuse(refusal::price);
        const std::uint32_t o = table_.get(id);
        if (o == npos) return refuse(refusal::unknown_id);
        const order_id nid = next_id_;
        if (!table_.can_take(nid)) return refuse(refusal::no_ids);
        order_rec& r = orders_[o];
        const std::uint32_t l = level_for(r.instrument, r.s, price);
        if (l == npos) return refuse(refusal::no_levels);
        unlink(o, l);
        table_.erase(id);
        r.id = nid;
        r.price = price;
        r.shares = shares;
        append(o, l);
        table_.put(nid, o);
        ++next_id_;
        return {refusal::none, nid};
    }

    // ------------------------------------- for the engine (note 003, D1)
    // An order is held by its record (a handle, npos for none) so a match
    // touches no order table; each of these is O(1).

    // The head of a side's best level: the order an incoming order meets
    // first, or npos.
    std::uint32_t best_head(instrument_t instrument, side s) const {
        const book& b = books_[instrument];
        const std::uint32_t l = s == side::buy ? b.bids.best(shared_) : b.asks.best(shared_);
        return l == npos ? npos : levels_[l].first;
    }
    std::uint32_t at(order_id id) const { return table_.get(id); }
    order_id id_at(std::uint32_t o) const { return orders_[o].id; }
    price_t price_at(std::uint32_t o) const { return orders_[o].price; }
    shares_t shares_at(std::uint32_t o) const { return orders_[o].shares; }
    instrument_t instrument_at(std::uint32_t o) const { return orders_[o].instrument; }
    side side_at(std::uint32_t o) const { return orders_[o].s; }
    // Shares off a resting order (1 to all it has), its place kept; at zero
    // it is removed, and its level if emptied. Its shares left.
    shares_t reduce(std::uint32_t o, shares_t shares) {
        order_rec& r = orders_[o];
        if (shares == r.shares) {
            remove(o);
            return 0;
        }
        r.shares -= shares;
        levels_[r.level].shares -= shares;
        return r.shares;
    }
    // Whether an order could rest at `price` now, as `add` would find it
    // (capacity only: an ID, a record, a level); the first lack, or none.
    refusal can_rest(instrument_t instrument, side s, price_t price) const {
        if (!table_.can_take(next_id_)) return refusal::no_ids;
        if (free_order_ == npos) return refusal::no_orders;
        const book& b = books_[instrument];
        if ((s == side::buy ? b.bids.find(shared_, price) : b.asks.find(shared_, price)) != npos) return refusal::none;
        const bool room = free_level_ != npos && (s == side::buy ? b.bids.can_insert(shared_) : b.asks.can_insert(shared_));
        return room ? refusal::none : refusal::no_levels;
    }
    // The next ID taken by an order that does not rest (D4: an IOC, or one
    // filled at entry); `add` takes it otherwise.
    void skip_id() { ++next_id_; }
    std::size_t instruments() const { return books_.size(); }

    // ------------------------------------------------------------ inspection
    std::optional<order_view> find(order_id id) const {
        const std::uint32_t o = table_.get(id);
        if (o == npos) return std::nullopt;
        const order_rec& r = orders_[o];
        return order_view{r.id, r.instrument, r.s, r.price, r.shares};
    }
    // One side's levels, best first, each with its queue (for tests).
    std::vector<level_state> state(instrument_t instrument, side s) const {
        std::vector<level_state> out;
        const auto one = [&](std::uint32_t l) {
            const level_rec& lv = levels_[l];
            level_state st{lv.price, lv.shares, lv.count, {}};
            for (std::uint32_t o = lv.first; o != npos; o = orders_[o].next) st.queue.push_back({orders_[o].id, orders_[o].shares});
            out.push_back(std::move(st));
        };
        const book& b = books_[instrument];
        if (s == side::buy) b.bids.each(shared_, one);
        else b.asks.each(shared_, one);
        return out;
    }
    // One level, with its queue, if the side has it (for tests).
    std::optional<level_state> level(instrument_t instrument, side s, price_t price) const {
        const book& b = books_[instrument];
        const std::uint32_t l = s == side::buy ? b.bids.find(shared_, price) : b.asks.find(shared_, price);
        if (l == npos) return std::nullopt;
        const level_rec& lv = levels_[l];
        level_state st{lv.price, lv.shares, lv.count, {}};
        for (std::uint32_t o = lv.first; o != npos; o = orders_[o].next) st.queue.push_back({orders_[o].id, orders_[o].shares});
        return st;
    }
    std::size_t live_orders() const { return live_orders_; }
    std::size_t live_levels() const { return live_levels_; }
    std::size_t peak_orders() const { return peak_orders_; }
    std::size_t peak_levels() const { return peak_levels_; }
    std::uint64_t refused(refusal r) const { return refused_[static_cast<int>(r)]; }
    order_id next_id() const { return next_id_; }
    static constexpr const char* levels_name = Levels::name;
    static constexpr const char* table_name = Table::name;

private:
    struct order_rec {
        order_id id;
        price_t price;
        shares_t shares;
        std::uint32_t level, prev, next;  // `next` links the free list too
        instrument_t instrument;
        me::side s;
    };
    struct level_rec {
        price_t price;
        std::uint32_t count;
        std::uint64_t shares;
        std::uint32_t first, last;  // `first` links the free list too
    };
    struct book {
        explicit book(typename Levels::shared& s) : bids(s), asks(s) {}
        typename Levels::template side<true> bids;
        typename Levels::template side<false> asks;
    };

    answer refuse(refusal r) {
        ++refused_[static_cast<int>(r)];
        return {r, 0};
    }

    // The level for `price` on the side, made if there is none; npos past
    // capacity (nothing changed).
    std::uint32_t level_for(instrument_t instrument, side s, price_t price) {
        book& b = books_[instrument];
        const std::uint32_t found = s == side::buy ? b.bids.find(shared_, price) : b.asks.find(shared_, price);
        if (found != npos) return found;
        if (free_level_ == npos) return npos;
        const std::uint32_t l = free_level_;
        const bool in = s == side::buy ? b.bids.insert(shared_, price, l) : b.asks.insert(shared_, price, l);
        if (!in) return npos;
        free_level_ = levels_[l].first;
        levels_[l] = {price, 0, 0, npos, npos};
        if (++live_levels_ > peak_levels_) peak_levels_ = live_levels_;
        return l;
    }

    void append(std::uint32_t o, std::uint32_t l) {
        level_rec& lv = levels_[l];
        order_rec& r = orders_[o];
        r.level = l;
        r.next = npos;
        r.prev = lv.last;
        if (lv.last != npos) orders_[lv.last].next = o;
        else lv.first = o;
        lv.last = o;
        ++lv.count;
        lv.shares += r.shares;
    }

    // Out of its queue; its level freed if left empty, unless it is `keep`.
    void unlink(std::uint32_t o, std::uint32_t keep) {
        order_rec& r = orders_[o];
        level_rec& lv = levels_[r.level];
        if (r.prev != npos) orders_[r.prev].next = r.next;
        else lv.first = r.next;
        if (r.next != npos) orders_[r.next].prev = r.prev;
        else lv.last = r.prev;
        --lv.count;
        lv.shares -= r.shares;
        if (lv.count == 0 && r.level != keep) {
            book& b = books_[r.instrument];
            if (r.s == side::buy) b.bids.erase(shared_, lv.price);
            else b.asks.erase(shared_, lv.price);
            lv.first = free_level_;
            free_level_ = r.level;
            --live_levels_;
        }
    }

    void remove(std::uint32_t o) {
        unlink(o, npos);
        table_.erase(orders_[o].id);
        orders_[o].next = free_order_;
        free_order_ = o;
        --live_orders_;
    }

    config cfg_;
    typename Levels::shared shared_;
    Table table_;
    table<order_rec> orders_;
    table<level_rec> levels_;
    std::vector<book> books_;
    std::uint32_t free_order_ = npos, free_level_ = npos;
    order_id next_id_ = 1;
    std::size_t live_orders_ = 0, live_levels_ = 0, peak_orders_ = 0, peak_levels_ = 0;
    std::uint64_t refused_[refusals] = {};
};

// The default (note 002, S6): levels A, table A, the fastest at every
// percentile over the day.
using book = market<levels_sorted, table_direct>;

}  // namespace me
