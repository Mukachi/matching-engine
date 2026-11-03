// S3 of note 002: the operations on every combination of D1 and D2, compiled
// as they are used, for check_hot_path.py to read. Never run.
// Design: docs/design/002-book.md
#include <me/market.hpp>

using namespace me;

#define SUBJECTS(tag, Levels, Table)                                                                                                  \
    market<Levels, Table>* m_##tag = nullptr;                                                                                       \
    extern "C" __attribute__((noinline)) answer me_add_##tag(instrument_t i, side s, price_t p, shares_t q) { return m_##tag->add(i, s, p, q); } \
    extern "C" __attribute__((noinline)) answer me_cancel_part_##tag(order_id id, shares_t q) { return m_##tag->cancel(id, q); }    \
    extern "C" __attribute__((noinline)) answer me_cancel_##tag(order_id id) { return m_##tag->cancel(id); }                       \
    extern "C" __attribute__((noinline)) answer me_replace_##tag(order_id id, price_t p, shares_t q) { return m_##tag->replace(id, p, q); }

SUBJECTS(aa, levels_sorted, table_direct)
SUBJECTS(ab, levels_sorted, table_hash)
SUBJECTS(ba, levels_map, table_direct)
SUBJECTS(bb, levels_map, table_hash)
SUBJECTS(ac, levels_sorted, table_low)
SUBJECTS(bc, levels_map, table_low)

int main(int argc, char**) {
    if (argc > 99) {  // never: keeps the subjects
        me_add_aa(0, side::buy, 1, 1), me_cancel_part_aa(1, 1), me_cancel_aa(1), me_replace_aa(1, 1, 1);
        me_add_ab(0, side::buy, 1, 1), me_cancel_part_ab(1, 1), me_cancel_ab(1), me_replace_ab(1, 1, 1);
        me_add_ba(0, side::buy, 1, 1), me_cancel_part_ba(1, 1), me_cancel_ba(1), me_replace_ba(1, 1, 1);
        me_add_bb(0, side::buy, 1, 1), me_cancel_part_bb(1, 1), me_cancel_bb(1), me_replace_bb(1, 1, 1);
        me_add_ac(0, side::buy, 1, 1), me_cancel_part_ac(1, 1), me_cancel_ac(1), me_replace_ac(1, 1, 1);
        me_add_bc(0, side::buy, 1, 1), me_cancel_part_bc(1, 1), me_cancel_bc(1), me_replace_bc(1, 1, 1);
    }
    return 0;
}
