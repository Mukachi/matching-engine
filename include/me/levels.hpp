// One side of a book's price levels (note 002, D2), two ways, each mapping a
// price to its level's index in the market's level pool and giving the best:
//   A  a sorted array, best at the end (most changes are near the best), in
//      chunks of one arena shared by every book, doubling as a side grows; so
//      memory follows the levels live, not instruments x the deepest book
//   B  std::map over a pool allocator reserved at start: the reference's
//      container, without its allocations
// Bid sides keep higher prices better, ask sides lower. Both are fixed and
// pre-faulted at start (D4); an insert past capacity returns false.
// Design: docs/design/002-book.md
#pragma once

#include <me/memory.hpp>
#include <me/types.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <type_traits>

namespace me {

// ------------------------------------------------------------------- A
// Chunks of 8 << k entries (k = 0..11: 8 to 16,384 levels a side); a freed
// chunk goes on its size's list. An entry is the price in the high half and
// the level's index in the low.
class level_arena {
public:
    static constexpr int classes = 12;
    level_arena(std::size_t entries, bool prefault) : size_(entries), mem_(zeroed<std::uint64_t>(entries, prefault)) {
        for (auto& h : free_) h = npos;
    }
    static std::uint32_t capacity(int k) { return 8u << k; }
    std::uint32_t alloc(int k) {
        if (k >= classes) return npos;
        if (free_[k] != npos) {
            const std::uint32_t off = free_[k];
            free_[k] = static_cast<std::uint32_t>(mem_[off]);
            return off;
        }
        if (used_ + capacity(k) > size_) return npos;
        const auto off = static_cast<std::uint32_t>(used_);
        used_ += capacity(k);
        return off;
    }
    void release(std::uint32_t off, int k) {
        mem_[off] = free_[k];
        free_[k] = off;
    }
    std::uint64_t* at(std::uint32_t off) { return &mem_[off]; }
    const std::uint64_t* at(std::uint32_t off) const { return &mem_[off]; }

private:
    std::size_t size_, used_ = 0;
    table<std::uint64_t> mem_;
    std::uint32_t free_[classes];
};

struct levels_sorted {
    static constexpr const char* name = "A: sorted array";
    struct shared {
        explicit shared(const config& c) : arena(c.level_arena, c.prefault) {}
        level_arena arena;
    };
    template <bool Bid>
    class side {
    public:
        explicit side(shared&) {}
        std::uint32_t find(const shared& s, price_t price) const {
            const std::uint32_t pos = lower(s.arena, price);
            return pos < n_ && price_of(s.arena.at(off_)[pos]) == price ? index_of(s.arena.at(off_)[pos]) : npos;
        }
        bool insert(shared& s, price_t price, std::uint32_t i) {
            level_arena& a = s.arena;
            if (off_ == npos || n_ == level_arena::capacity(k_)) {
                const int k = off_ == npos ? 0 : k_ + 1;
                const std::uint32_t off = a.alloc(k);
                if (off == npos) return false;
                if (off_ != npos) {
                    std::memcpy(a.at(off), a.at(off_), n_ * sizeof(std::uint64_t));
                    a.release(off_, k_);
                }
                off_ = off;
                k_ = k;
            }
            const std::uint32_t pos = lower(a, price);
            std::uint64_t* e = a.at(off_);
            std::memmove(e + pos + 1, e + pos, (n_ - pos) * sizeof(std::uint64_t));
            e[pos] = std::uint64_t{price} << 32 | i;
            ++n_;
            return true;
        }
        void erase(shared& s, price_t price) {
            level_arena& a = s.arena;
            const std::uint32_t pos = lower(a, price);
            if (pos >= n_ || price_of(a.at(off_)[pos]) != price) return;
            std::uint64_t* e = a.at(off_);
            std::memmove(e + pos, e + pos + 1, (n_ - pos - 1) * sizeof(std::uint64_t));
            if (--n_ == 0) {
                a.release(off_, k_);
                off_ = npos;
                k_ = 0;
            }
        }
        std::uint32_t best(const shared& s) const { return n_ ? index_of(s.arena.at(off_)[n_ - 1]) : npos; }
        std::uint32_t size() const { return n_; }
        // Best first.
        template <class F>
        void each(const shared& s, F&& f) const {
            for (std::uint32_t j = n_; j-- > 0;) f(index_of(s.arena.at(off_)[j]));
        }

    private:
        static price_t price_of(std::uint64_t e) { return static_cast<price_t>(e >> 32); }
        static std::uint32_t index_of(std::uint64_t e) { return static_cast<std::uint32_t>(e); }
        // Kept worst first: bids ascending, asks descending. The first position
        // whose price is not worse than `price`.
        std::uint32_t lower(const level_arena& a, price_t price) const {
            if (n_ == 0) return 0;
            const std::uint64_t* e = a.at(off_);
            std::uint32_t lo = 0, hi = n_;
            while (lo < hi) {
                const std::uint32_t mid = (lo + hi) / 2;
                const price_t p = price_of(e[mid]);
                if (Bid ? p < price : p > price) lo = mid + 1;
                else hi = mid;
            }
            return lo;
        }
        std::uint32_t off_ = npos, n_ = 0;
        int k_ = 0;
    };
};

// ------------------------------------------------------------------- B
// Fixed blocks for the maps' nodes, reserved at start; the maps allocate only
// from here, one node a level (libstdc++ keeps a map's header in the map).
class node_pool {
public:
    static constexpr std::size_t block = 64;
    node_pool(std::size_t n, bool prefault) : n_(n), mem_(zeroed<cell>(n, prefault)) {}
    void* get() {
        if (free_) {
            cell* c = free_;
            free_ = c->next;
            return c;
        }
        if (used_ == n_) __builtin_trap();  // never: the market checks its level capacity first (a trap: a throw allocates)
        return &mem_[used_++];
    }
    void put(void* p) {
        cell* c = static_cast<cell*>(p);
        c->next = free_;
        free_ = c;
    }

private:
    union cell {
        cell* next;
        alignas(std::max_align_t) unsigned char bytes[block];
    };
    std::size_t n_, used_ = 0;
    table<cell> mem_;
    cell* free_ = nullptr;
};

template <class T>
struct pool_allocator {
    using value_type = T;
    node_pool* pool;
    explicit pool_allocator(node_pool* p) : pool(p) {}
    template <class U>
    pool_allocator(const pool_allocator<U>& o) : pool(o.pool) {}
    T* allocate(std::size_t n) {
        static_assert(sizeof(T) <= node_pool::block);
        if (n != 1) __builtin_trap();  // a map's nodes, one at a time
        return static_cast<T*>(pool->get());
    }
    void deallocate(T* p, std::size_t) { pool->put(p); }
    template <class U>
    bool operator==(const pool_allocator<U>& o) const {
        return pool == o.pool;
    }
};

struct levels_map {
    static constexpr const char* name = "B: std::map, pooled";
    struct shared {
        explicit shared(const config& c) : nodes(c.levels, c.prefault) {}
        node_pool nodes;
    };
    template <bool Bid>
    class side {
        using cmp = std::conditional_t<Bid, std::greater<>, std::less<>>;
        using alloc = pool_allocator<std::pair<const price_t, std::uint32_t>>;

    public:
        explicit side(shared& s) : m_(cmp{}, alloc(&s.nodes)) {}
        std::uint32_t find(const shared&, price_t price) const {
            const auto it = m_.find(price);
            return it == m_.end() ? npos : it->second;
        }
        bool insert(shared&, price_t price, std::uint32_t i) {
            m_.emplace(price, i);
            return true;
        }
        void erase(shared&, price_t price) { m_.erase(price); }
        std::uint32_t best(const shared&) const { return m_.empty() ? npos : m_.begin()->second; }
        std::uint32_t size() const { return static_cast<std::uint32_t>(m_.size()); }
        template <class F>
        void each(const shared&, F&& f) const {
            for (const auto& [p, i] : m_) f(i);
        }

    private:
        std::map<price_t, std::uint32_t, cmp, alloc> m_;
    };
};

}  // namespace me
