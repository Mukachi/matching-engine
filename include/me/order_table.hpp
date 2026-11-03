// From an order's ID to its record in the market's pool (note 002, D1), two
// ways:
//   A  a direct array indexed by ID: the engine's IDs are dense by
//      construction, so no hashing and no probe; it grows with every order
//      entered (fixed capacity, pre-faulted; past it, an add is refused)
//   B  open addressing over a fixed table sized for the orders live at once
//      (twice them, as a power of two), linear probing, deletion by backward
//      shift (no tombstones): memory bounded by live orders
// Design: docs/design/002-book.md
#pragma once

#include <me/memory.hpp>
#include <me/types.hpp>

#include <bit>
#include <cstdint>

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
        std::size_t i = home(id);
        while (e_[i].id != 0) i = (i + 1) & mask_;
        e_[i] = {id, slot};
    }
    std::uint32_t get(order_id id) const {
        if (id == 0) return npos;
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            if (e_[i].id == id) return e_[i].slot;
            if (e_[i].id == 0) return npos;
        }
    }
    void erase(order_id id) {
        std::size_t i = home(id);
        while (e_[i].id != id) i = (i + 1) & mask_;
        // Backward shift: pull each later entry of the run into the hole if
        // its home is not between the hole and itself.
        for (std::size_t j = (i + 1) & mask_; e_[j].id != 0; j = (j + 1) & mask_) {
            const std::size_t h = home(e_[j].id);
            if (((j - h) & mask_) >= ((j - i) & mask_)) {
                e_[i] = e_[j];
                i = j;
            }
        }
        e_[i] = {0, 0};
    }

private:
    struct entry {
        order_id id;  // 0 is empty
        std::uint32_t slot;
    };
    std::size_t home(order_id id) const { return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_); }
    std::size_t mask_;
    int shift_;
    table<entry> e_;
};

}  // namespace me
