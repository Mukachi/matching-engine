// The reference books (note 002): the same rules as `market`, written for
// clarity with standard containers, an ordered map of price to a list of
// orders per side, and a hash map from ID to the order's place. The oracle
// for the fast books and their baseline. No capacity: it never refuses for
// lack of room.
// Design: docs/design/002-book.md
#pragma once

#include <me/market.hpp>
#include <me/types.hpp>

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me {

class reference_market {
public:
    explicit reference_market(const config& c = {}) : instruments_(c.instruments) {}

    answer add(instrument_t instrument, side s, price_t price, shares_t shares) {
        if (!valid_size(shares)) return refuse(refusal::size);
        if (!valid_price(price)) return refuse(refusal::price);
        if (instrument >= instruments_) return refuse(refusal::instrument);
        const order_id id = next_id_++;
        rest(instrument, s, price, {id, shares});
        return {refusal::none, id};
    }

    answer cancel(order_id id, shares_t shares) {
        const auto it = where_.find(id);
        if (it == where_.end()) return refuse(refusal::unknown_id);
        if (shares == 0) return refuse(refusal::size);
        queued& q = *it->second.at;
        if (shares > q.shares) return refuse(refusal::too_many);
        if (shares == q.shares) erase(it);
        else q.shares -= shares;
        return {refusal::none, id};
    }

    answer cancel(order_id id) {
        const auto it = where_.find(id);
        if (it == where_.end()) return refuse(refusal::unknown_id);
        erase(it);
        return {refusal::none, id};
    }

    answer replace(order_id id, price_t price, shares_t shares) {
        if (!valid_size(shares)) return refuse(refusal::size);
        if (!valid_price(price)) return refuse(refusal::price);
        const auto it = where_.find(id);
        if (it == where_.end()) return refuse(refusal::unknown_id);
        const place p = it->second;
        erase(it);
        const order_id nid = next_id_++;
        rest(p.instrument, p.s, price, {nid, shares});
        return {refusal::none, nid};
    }

    std::optional<order_view> find(order_id id) const {
        const auto it = where_.find(id);
        if (it == where_.end()) return std::nullopt;
        return order_view{id, it->second.instrument, it->second.s, it->second.price, it->second.at->shares};
    }
    std::vector<level_state> state(instrument_t instrument, side s) const {
        std::vector<level_state> out;
        const auto bit = books_.find(instrument);
        if (bit == books_.end()) return out;
        const auto one = [&](price_t price, const std::list<queued>& q) {
            level_state st{price, 0, 0, {q.begin(), q.end()}};
            for (const queued& x : q) st.shares += x.shares;
            st.count = static_cast<std::uint32_t>(q.size());
            out.push_back(std::move(st));
        };
        if (s == side::buy)
            for (const auto& [p, q] : bit->second.bids) one(p, q);
        else
            for (const auto& [p, q] : bit->second.asks) one(p, q);
        return out;
    }
    std::optional<level_state> level(instrument_t instrument, side s, price_t price) const {
        const auto bit = books_.find(instrument);
        if (bit == books_.end()) return std::nullopt;
        const auto one = [&](const auto& side_map) -> std::optional<level_state> {
            const auto it = side_map.find(price);
            if (it == side_map.end()) return std::nullopt;
            level_state st{price, 0, static_cast<std::uint32_t>(it->second.size()), {it->second.begin(), it->second.end()}};
            for (const queued& x : it->second) st.shares += x.shares;
            return st;
        };
        return s == side::buy ? one(bit->second.bids) : one(bit->second.asks);
    }
    // For the reference engine (note 003): the order an incoming order meets
    // first on a side (the front of its best level), and its price.
    std::optional<std::pair<price_t, queued>> best(instrument_t instrument, side s) const {
        const auto bit = books_.find(instrument);
        if (bit == books_.end()) return std::nullopt;
        if (s == side::buy) {
            if (bit->second.bids.empty()) return std::nullopt;
            const auto& [p, q] = *bit->second.bids.begin();
            return std::pair{p, q.front()};
        }
        if (bit->second.asks.empty()) return std::nullopt;
        const auto& [p, q] = *bit->second.asks.begin();
        return std::pair{p, q.front()};
    }
    void skip_id() { ++next_id_; }
    std::size_t instruments() const { return instruments_; }
    std::size_t live_orders() const { return where_.size(); }
    std::uint64_t refused(refusal r) const { return refused_[static_cast<int>(r)]; }
    order_id next_id() const { return next_id_; }

private:
    struct place {
        instrument_t instrument;
        me::side s;
        price_t price;
        std::list<queued>::iterator at;
    };
    struct book {
        std::map<price_t, std::list<queued>, std::greater<>> bids;  // best (highest) first
        std::map<price_t, std::list<queued>, std::less<>> asks;     // best (lowest) first
    };

    answer refuse(refusal r) {
        ++refused_[static_cast<int>(r)];
        return {r, 0};
    }
    void rest(instrument_t instrument, side s, price_t price, queued q) {
        book& b = books_[instrument];
        std::list<queued>& level = s == side::buy ? b.bids[price] : b.asks[price];
        level.push_back(q);
        where_[q.id] = {instrument, s, price, std::prev(level.end())};
    }
    void erase(std::unordered_map<order_id, place>::iterator it) {
        const place& p = it->second;
        book& b = books_[p.instrument];
        if (p.s == side::buy) {
            auto lv = b.bids.find(p.price);
            lv->second.erase(p.at);
            if (lv->second.empty()) b.bids.erase(lv);
        } else {
            auto lv = b.asks.find(p.price);
            lv->second.erase(p.at);
            if (lv->second.empty()) b.asks.erase(lv);
        }
        where_.erase(it);
    }

    std::size_t instruments_;
    std::unordered_map<instrument_t, book> books_;
    std::unordered_map<order_id, place> where_;
    order_id next_id_ = 1;
    std::uint64_t refused_[refusals] = {};
};

}  // namespace me
