// S3 of notes 002 and 003: the book's operations, and the engine's (matching
// included) with a sink that counts, on every combination of note 002's D1 and
// D2, compiled as they are used, for check_hot_path.py to read. Never run.
// Design: docs/design/002-book.md, docs/design/003-matching.md
#include <me/engine.hpp>
#include <me/market.hpp>

#include <cstdint>

using namespace me;

// A sink as the benchmark's: it counts.
struct counting {
    std::uint64_t n = 0, shares = 0;
    void accepted(order_id) { ++n; }
    void refused(refusal) { ++n; }
    void trade(const trade_report& t) { shares += t.shares; }
    void rested(order_id, price_t, shares_t) { ++n; }
    void cancelled(order_id, shares_t s, cause) { shares += s; }
};

#define SUBJECTS(tag, Levels, Table)                                                                                                  \
    market<Levels, Table>* m_##tag = nullptr;                                                                                       \
    extern "C" __attribute__((noinline)) answer me_add_##tag(instrument_t i, side s, price_t p, shares_t q) { return m_##tag->add(i, s, p, q); } \
    extern "C" __attribute__((noinline)) answer me_cancel_part_##tag(order_id id, shares_t q) { return m_##tag->cancel(id, q); }    \
    extern "C" __attribute__((noinline)) answer me_cancel_##tag(order_id id) { return m_##tag->cancel(id); }                       \
    extern "C" __attribute__((noinline)) answer me_replace_##tag(order_id id, price_t p, shares_t q) { return m_##tag->replace(id, p, q); }

#define ENGINE(tag, Levels, Table)                                                                                                    \
    engine<market<Levels, Table>, counting>* e_##tag = nullptr;                                                                     \
    extern "C" __attribute__((noinline)) void me_enter_##tag(instrument_t i, side s, price_t p, shares_t q, tif t) { e_##tag->enter(i, s, p, q, t); } \
    extern "C" __attribute__((noinline)) void me_ecancel_part_##tag(order_id id, shares_t q) { e_##tag->cancel(id, q); }            \
    extern "C" __attribute__((noinline)) void me_ecancel_##tag(order_id id) { e_##tag->cancel(id); }                               \
    extern "C" __attribute__((noinline)) void me_ereplace_##tag(order_id id, price_t p, shares_t q) { e_##tag->replace(id, p, q); }

SUBJECTS(aa, levels_sorted, table_direct)
SUBJECTS(ab, levels_sorted, table_hash)
SUBJECTS(ba, levels_map, table_direct)
SUBJECTS(bb, levels_map, table_hash)
ENGINE(aa, levels_sorted, table_direct)
ENGINE(ab, levels_sorted, table_hash)
ENGINE(ba, levels_map, table_direct)
ENGINE(bb, levels_map, table_hash)

int main(int argc, char**) {
    if (argc > 99) {  // never: keeps the subjects
        me_add_aa(0, side::buy, 1, 1), me_cancel_part_aa(1, 1), me_cancel_aa(1), me_replace_aa(1, 1, 1);
        me_add_ab(0, side::buy, 1, 1), me_cancel_part_ab(1, 1), me_cancel_ab(1), me_replace_ab(1, 1, 1);
        me_add_ba(0, side::buy, 1, 1), me_cancel_part_ba(1, 1), me_cancel_ba(1), me_replace_ba(1, 1, 1);
        me_add_bb(0, side::buy, 1, 1), me_cancel_part_bb(1, 1), me_cancel_bb(1), me_replace_bb(1, 1, 1);
        me_enter_aa(0, side::buy, 1, 1, tif::ioc), me_ecancel_part_aa(1, 1), me_ecancel_aa(1), me_ereplace_aa(1, 1, 1);
        me_enter_ab(0, side::buy, 1, 1, tif::ioc), me_ecancel_part_ab(1, 1), me_ecancel_ab(1), me_ereplace_ab(1, 1, 1);
        me_enter_ba(0, side::buy, 1, 1, tif::ioc), me_ecancel_part_ba(1, 1), me_ecancel_ba(1), me_ereplace_ba(1, 1, 1);
        me_enter_bb(0, side::buy, 1, 1, tif::ioc), me_ecancel_part_bb(1, 1), me_ecancel_bb(1), me_ereplace_bb(1, 1, 1);
    }
    return 0;
}
