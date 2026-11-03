// From an order's ID to its record in the market's pool (note 002, D1), two
// ways:
//   A  a direct array indexed by ID: the engine's IDs are dense by
//      construction, so no hashing and no probe; it grows with every order
//      entered (fixed capacity, pre-faulted; past it, an add is refused)
//   B  open addressing over a fixed table sized for the orders live at once
//      (twice them, as a power of two), linear probing in Robin Hood order
//      (an entry never sits farther from its home than the one after it),
//      deletion by backward shift (no tombstones): memory bounded by live
//      orders
// Robin Hood order lets a deletion stop at the first entry in its own home
// slot, and a missed lookup at the first entry nearer its home than the probe.
// A third form, the ID's low bits as the home slot, was built and dropped: the
// day's long-resting orders made runs thousands long (note 002's Review).
// Design: docs/design/002-book.md
#pragma once

#include <me/memory.hpp>
#include <me/types.hpp>

#include <bit>
#include <cstdint>
#include <utility>

namespace me {

struct table_direct {
    static constexpr const char* name = "A: direct array";
    explicit table_direct(const config& c) : n_(c.ids), slot_(zeroed<std::uint32_t>(c.ids, c.prefault)) {}
    bool can_take(order_id id) const { return id >= 1 && id <= n_; }
    void put(order_id id, std::uint32_t slot) { slot_[id - 1] = slot + 1; }
    std::uint32_t get(order_id id) const { return id >= 1 && id <= n_ ? slot_[id - 1] - 1 : npos; }
    void erase(order_id id) { slot_[id - 1] = 0; }

private:
    std::size_t n_;
    table<std::uint32_t> slot_;  // record + 1; 0 is none (and 0 - 1 is npos)
};

struct table_hash {
    static constexpr const char* name = "B: open addressing";
    explicit table_hash(const config& c)
        : mask_(pow2_at_least(2 * c.orders) - 1), shift_(64 - std::countr_zero(mask_ + 1)), e_(zeroed<entry>(mask_ + 1, c.prefault)) {}
    bool can_take(order_id id) const { return id >= 1; }
    void put(order_id id, std::uint32_t slot) {
        entry carry{id, slot};
        std::size_t d = 0;  // carry's distance from its home
        for (std::size_t i = home(id);; i = (i + 1) & mask_, ++d) {
            if (e_[i].id == 0) {
                e_[i] = carry;
                return;
            }
            const std::size_t r = distance(i);
            if (r < d) {  // the resident is nearer its home: carry takes its place
                std::swap(e_[i], carry);
                d = r;
            }
        }
    }
    std::uint32_t get(order_id id) const {
        if (id == 0) return npos;
        for (std::size_t i = home(id), d = 0;; i = (i + 1) & mask_, ++d) {
            if (e_[i].id == id) return e_[i].slot;
            if (e_[i].id == 0 || distance(i) < d) return npos;
        }
    }
    void erase(order_id id) {
        std::size_t i = home(id);
        while (e_[i].id != id) i = (i + 1) & mask_;
        // Backward shift: each later entry away from its home moves back one;
        // the first empty or at its home ends the run's shifted part.
        for (std::size_t j = (i + 1) & mask_; e_[j].id != 0 && distance(j) != 0; j = (j + 1) & mask_) {
            e_[i] = e_[j];
            i = j;
        }
        e_[i] = {0, 0};
    }

private:
    struct entry {
        order_id id;  // 0 is empty
        std::uint32_t slot;
    };
    std::size_t home(order_id id) const {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_);
    }
    std::size_t distance(std::size_t i) const { return (i - home(e_[i].id)) & mask_; }
    std::size_t mask_;
    int shift_;
    table<entry> e_;
};

}  // namespace me
