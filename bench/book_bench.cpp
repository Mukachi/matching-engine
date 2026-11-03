// The books' cost (note 002, S5): the reference and every combination of D1
// and D2 (default capacities), one after another on CPU 2.
//
// The day (D7): the sample day's displayed book events, converted once,
// before anything is timed, into operations carrying the engine's IDs (the
// conversion applies them to a fast book to know when an order is finished),
// so no timed window holds the mapping from ITCH's reference numbers. For each
// book, two passes, each with a fresh book:
//   timed: every operation between start() and stop(), unsubtracted, into a
//          histogram per operation (lockfree-ipc's), the first million
//          discarded as warm-up; beside an empty window ("none") for the
//          clock's own cost;
//   whole day: every operation untimed, as operations a second.
// And the most memory the book held: resident memory read every million
// operations in the timed pass (outside every window), its peak less what was
// resident before the book was built (the fast books' tables are faulted in at
// construction, so they count whole). The heap is handed back to the kernel
// (malloc_trim) before every pass, as the feed handler's benchmark found it
// must be.
//
// Synthetic cases (D7): one instrument, 1,000 levels a side of 10 orders each,
// a case at a time, 1,000,000 times each after 10,000 of warm-up; each timed
// operation followed by an untimed one that puts the book back. The book stays
// in cache, so these are each operation's cost at its best; the day's figures
// are the realistic ones.
// Run JSON through lockfree-ipc's harness. Design: docs/design/002-book.md
//
// Usage: book_bench [output.json] [day file]
#include "../tools/day_file.hpp"

#include <me/market.hpp>
#include <me/memory.hpp>
#include <me/reference_market.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace lfipc::bench;
using namespace me;

constexpr int cpu = 2;
constexpr std::size_t warmup = 1'000'000;
using hist = histogram<10, 40>;

// An operation of the day, with the engine's IDs already in it.
enum class op_type : std::uint8_t { add = 1, cancel_part = 2, cancel = 3, replace = 4 };
constexpr int op_types = 5;  // 1..4, and 0 for "all"
const char* const op_names[op_types] = {"all", "add", "cancel part", "cancel", "replace"};
struct op {
    order_id id;  // cancel and replace: the order's; add: the ID it will be given (checked)
    price_t price;
    shares_t shares;
    instrument_t instrument;
    op_type type;
    me::side s;
};
static_assert(sizeof(op) == 24);

std::size_t resident_kb() {
    std::ifstream f("/proc/self/status");
    std::string k;
    std::size_t v = 0;
    while (f >> k)
        if (k == "VmRSS:") {
            f >> v;
            return v;
        }
    return 0;
}

template <class M>
answer apply(M& m, const op& o) {
    switch (o.type) {
        case op_type::add: return m.add(o.instrument, o.s, o.price, o.shares);
        case op_type::cancel_part: return m.cancel(o.id, o.shares);
        case op_type::cancel: return m.cancel(o.id);
        case op_type::replace: return m.replace(o.id, o.price, o.shares);
    }
    return {};
}

// The day as operations: each Add an add, each Order Executed, Executed With
// Price and Order Cancel a cancel of part, each Order Delete a cancel, each
// Order Replace a replace (D7). The conversion's own book (levels A, table A)
// says when an order is finished; anything refused is counted and left out.
std::vector<op> read_day(const std::string& path, std::uint64_t& messages, std::uint64_t& refused) {
    day_file f(path);
    std::vector<op> ops;
    if (!f.open()) return ops;
    ops.reserve(280'000'000);
    auto conv = std::make_unique<market<levels_sorted, table_direct>>(config{.prefault = false});
    std::unordered_map<std::uint64_t, order_id> id_of;
    id_of.reserve(4'000'000);
    itchfh::message m;
    const auto mine = [&](std::uint64_t ref) {
        const auto it = id_of.find(ref);
        return it == id_of.end() ? order_id{0} : it->second;
    };
    while (f.next(m)) {
        op o{};
        o.instrument = m.locate;
        std::uint64_t ref = 0;
        switch (m.type) {
            case 'A':
            case 'F':
                o.type = op_type::add, o.s = m.A.side == 'B' ? side::buy : side::sell, o.price = m.A.price, o.shares = m.A.shares;
                ref = m.A.ref;
                break;
            case 'E':
            case 'C':
                o.type = op_type::cancel_part, o.shares = m.E.shares, ref = m.E.ref;
                break;
            case 'X':
                o.type = op_type::cancel_part, o.shares = m.X.shares, ref = m.X.ref;
                break;
            case 'D':
                o.type = op_type::cancel, ref = m.D.ref;
                break;
            case 'U':
                o.type = op_type::replace, o.price = m.U.price, o.shares = m.U.shares, ref = m.U.old_ref;
                break;
            default:
                continue;
        }
        if (o.type != op_type::add && !(o.id = mine(ref))) {
            ++refused;
            continue;
        }
        const answer a = apply(*conv, o);
        if (!a.ok()) {
            ++refused;
            continue;
        }
        if (o.type == op_type::add) {
            o.id = a.id;
            id_of[ref] = a.id;
        } else if (o.type == op_type::replace) {
            id_of.erase(ref);
            id_of[m.U.new_ref] = a.id;
        } else if (!conv->find(o.id)) {
            id_of.erase(ref);
        }
        ops.push_back(o);
    }
    messages = f.sequence();
    return ops;
}

struct day_result {
    std::string name, levels, table;
    std::vector<hist> h = std::vector<hist>(op_types);
    double seconds = 0, ops_per_s = 0, peak_resident_mb = 0;
    std::size_t peak_orders = 0, peak_levels = 0;
    bool clean = true;  // every answer as the conversion had it
};

template <class Make>
day_result day(const std::string& name, const char* levels, const char* table, std::span<const op> ops, Make make) {
    day_result r;
    r.name = name, r.levels = levels, r.table = table;
    {
        ::malloc_trim(0);
        const std::size_t before = resident_kb();
        std::size_t peak = before;
        auto b = make();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (i % 1'000'000 == 0) peak = std::max(peak, resident_kb());
            const ticks t0 = start();
            const answer a = apply(*b, ops[i]);
            const ticks t1 = stop();
            r.clean &= a.ok() && (ops[i].type != op_type::add || a.id == ops[i].id);
            if (i < warmup) continue;
            r.h[static_cast<int>(ops[i].type)].record(t1 - t0);
            r.h[0].record(t1 - t0);
        }
        peak = std::max(peak, resident_kb());
        r.peak_resident_mb = static_cast<double>(peak - before) / 1024.0;
    }
    ::malloc_trim(0);
    auto b = make();
    const auto w0 = std::chrono::steady_clock::now();
    for (const op& o : ops) r.clean &= apply(*b, o).ok();
    const auto w1 = std::chrono::steady_clock::now();
    r.seconds = std::chrono::duration<double>(w1 - w0).count();
    r.ops_per_s = static_cast<double>(ops.size()) / r.seconds;
    if constexpr (requires { b->peak_orders(); }) {
        r.peak_orders = b->peak_orders();
        r.peak_levels = b->peak_levels();
    }
    std::printf("day: %-42s %7.2f M ops/s  %6.1f s  %8.0f MB%s\n", name.c_str(), r.ops_per_s / 1e6, r.seconds, r.peak_resident_mb,
                r.clean ? "" : "  REFUSED SOME");
    std::fflush(stdout);
    return r;
}

// ------------------------------------------------------------ synthetic
constexpr int cases = 10;
const char* const case_names[cases] = {"add at the best level", "add at a new level far from the best", "add at a new best level",
                                       "cancel at a level's head",  "cancel in a level's middle",          "cancel at a level's tail",
                                       "cancel part",              "replace in its level",               "replace to another level",
                                       "cancel part to nothing"};
constexpr std::size_t synthetic_reps = 1'000'000, synthetic_warmup = 10'000;
constexpr int depth = 1000, per_level = 10;
constexpr price_t mid = 1'000'000;  // $100.0000; a cent a level

struct synthetic_result {
    std::vector<hist> h = std::vector<hist>(cases);
    bool clean = true;
};

template <class M>
synthetic_result synthetic(M& m) {
    synthetic_result r;
    const auto bid = [](int level) { return mid - 100 * static_cast<price_t>(level + 1); };  // level 0 the best
    // Each level's orders, front first, as the book has them.
    std::vector<std::vector<order_id>> q(depth);
    for (int l = 0; l < depth; ++l)
        for (int k = 0; k < per_level; ++k) q[l].push_back(m.add(0, side::buy, bid(l), 100).id);
    for (int l = 0; l < depth; ++l) m.add(0, side::sell, mid + 100 * static_cast<price_t>(l + 1), 100);
    const auto timed = [&](int c, std::size_t i, auto&& f) {
        const ticks t0 = start();
        const answer a = f();
        const ticks t1 = stop();
        r.clean &= a.ok();
        if (i >= synthetic_warmup) r.h[c].record(t1 - t0);
        return a;
    };
    const std::size_t n = synthetic_reps + synthetic_warmup;
    for (std::size_t i = 0; i < n; ++i) {  // 0: join the best level, then leave it
        const answer a = timed(0, i, [&] { return m.add(0, side::buy, bid(0), 100); });
        m.cancel(a.id);
    }
    for (std::size_t i = 0; i < n; ++i) {  // 1: a new level beyond the deepest, then gone
        const answer a = timed(1, i, [&] { return m.add(0, side::buy, bid(depth + 10), 100); });
        m.cancel(a.id);
    }
    for (std::size_t i = 0; i < n; ++i) {  // 2: a new best level inside the spread, then gone
        const answer a = timed(2, i, [&] { return m.add(0, side::buy, mid - 50, 100); });
        m.cancel(a.id);
    }
    // 3, 4, 5: cancel at a level's head, middle and tail (levels in turn), the
    // order put back at the tail untimed; the queue kept as the book has it.
    for (int c = 3; c <= 5; ++c)
        for (std::size_t i = 0; i < n; ++i) {
            const int l = static_cast<int>(i % depth);
            const std::size_t at = c == 3 ? 0 : c == 4 ? per_level / 2 : per_level - 1;
            const order_id id = q[l][at];
            timed(c, i, [&] { return m.cancel(id); });
            q[l].erase(q[l].begin() + static_cast<std::ptrdiff_t>(at));
            q[l].push_back(m.add(0, side::buy, bid(l), 100).id);
        }
    // 6: cancel part, a share at a time from orders of 999,999 at the best
    // level, each replaced by a fresh one before it runs out.
    {
        order_id big = m.add(0, side::buy, bid(0), max_shares).id;
        shares_t left = max_shares;
        for (std::size_t i = 0; i < n; ++i) {
            timed(6, i, [&] { return m.cancel(big, 1); });
            if (--left == 1) {
                m.cancel(big);
                big = m.add(0, side::buy, bid(0), max_shares).id;
                left = max_shares;
            }
        }
        m.cancel(big);
    }
    // 7: replace in its level (to the back), the level's head each time.
    for (std::size_t i = 0; i < n; ++i) {
        const int l = static_cast<int>(i % depth);
        const answer a = timed(7, i, [&] { return m.replace(q[l].front(), bid(l), 100); });
        q[l].erase(q[l].begin());
        q[l].push_back(a.id);
    }
    // 8: replace to another level, there and back (the back each time).
    for (std::size_t i = 0; i < n; ++i) {
        const int l = static_cast<int>(i % (depth - 1));
        const answer a = timed(8, i, [&] { return m.replace(q[l].front(), bid(l + 1), 100); });
        q[l].erase(q[l].begin());
        const answer b = m.replace(a.id, bid(l), 100);
        q[l].push_back(b.id);
    }
    // 9: cancel part to nothing: the whole of an order's shares, so it is removed.
    for (std::size_t i = 0; i < n; ++i) {
        const int l = static_cast<int>(i % depth);
        const order_id id = q[l].back();
        timed(9, i, [&] { return m.cancel(id, 100); });
        q[l].back() = m.add(0, side::buy, bid(l), 100).id;
    }
    return r;
}

struct book_result {
    day_result d;
    synthetic_result s;
};

template <class M>
book_result one(const std::string& name, const char* levels, const char* table, std::span<const op> ops) {
    book_result b;
    b.d = day(name, levels, table, ops, [] { return std::make_unique<M>(); });
    ::malloc_trim(0);
    auto m = std::make_unique<M>();
    b.s = synthetic(*m);
    std::printf("synthetic: %-36s%s\n", name.c_str(), b.s.clean ? "" : "  REFUSED SOME");
    std::fflush(stdout);
    return b;
}

template <class L, class T>
book_result fast(std::span<const op> ops) {
    return one<market<L, T>>(std::string("levels ") + L::name + ", table " + T::name, L::name, T::name, ops);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : ME_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "book");
    if (run.refused) return run.finish(false, "book_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "book_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "book_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    std::uint64_t messages = 0, left_out = 0;
    const std::vector<op> read = read_day(path, messages, left_out);
    if (read.empty()) {
        std::fprintf(stderr, "book_bench: %s: no sample day (see README)\n", path.c_str());
        write_clock_block(run.w, clk);
        return run.finish(false, "book_bench");
    }
    auto copy = zeroed<op>(read.size(), true);  // on CPU 2's node: the harness bound memory there
    std::copy(read.begin(), read.end(), copy.get());
    const std::span<const op> ops(copy.get(), read.size());
    std::printf("the day: %llu messages, %zu operations, %llu left out\n", static_cast<unsigned long long>(messages), ops.size(),
                static_cast<unsigned long long>(left_out));

    hist none;
    for (std::size_t i = 0; i < 10'000'000; ++i) {
        const ticks t0 = start();
        const ticks t1 = stop();
        if (i >= warmup) none.record(t1 - t0);
    }

    std::vector<book_result> results;
    results.push_back(one<reference_market>("reference", "reference", "reference", ops));
    results.push_back(fast<levels_sorted, table_direct>(ops));
    results.push_back(fast<levels_sorted, table_hash>(ops));
    results.push_back(fast<levels_map, table_direct>(ops));
    results.push_back(fast<levels_map, table_hash>(ops));
    results.push_back(fast<levels_sorted, table_low>(ops));
    results.push_back(fast<levels_map, table_low>(ops));

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
    const auto write_hist = [&](const hist& h) {
        run.w.member("count", h.count());
        run.w.member("p50_ns", ns(h.percentile(50)));
        run.w.member("p99_ns", ns(h.percentile(99)));
        run.w.member("p999_ns", ns(h.percentile(99.9)));
        run.w.member("max_ns", static_cast<double>(h.max()) / tpn);
    };
    bool ok = true;
    run.w.key("book");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", "01302020.NASDAQ_ITCH50.gz");
    run.w.member("messages", messages);
    run.w.member("operations", static_cast<std::uint64_t>(ops.size()));
    run.w.member("left_out", left_out);
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("synthetic_repetitions", static_cast<std::uint64_t>(synthetic_reps));
    run.w.member("synthetic_warmup", static_cast<std::uint64_t>(synthetic_warmup));
    run.w.member("synthetic_book", "one instrument, 1,000 levels a side of 10 orders, a cent apart");
    run.w.member("lockfree_ipc_commit", ME_LFIPC_COMMIT);
    run.w.member("itch_feed_handler_commit", ME_ITCHFH_COMMIT);
    run.w.key("none");
    run.w.begin_object();
    write_hist(none);
    run.w.end_object();
    run.w.key("books");
    run.w.begin_array();
    for (const book_result& b : results) {
        const day_result& r = b.d;
        ok &= r.clean && b.s.clean;
        run.w.begin_object();
        run.w.member("name", r.name);
        run.w.member("levels", r.levels);
        run.w.member("table", r.table);
        run.w.member("nothing_refused", r.clean && b.s.clean);
        run.w.member("ops_per_s", r.ops_per_s);
        run.w.member("seconds", r.seconds);
        run.w.member("peak_resident_mb", r.peak_resident_mb);
        run.w.member("peak_orders", static_cast<std::uint64_t>(r.peak_orders));
        run.w.member("peak_levels", static_cast<std::uint64_t>(r.peak_levels));
        run.w.key("day");
        run.w.begin_array();
        for (int t = 0; t < op_types; ++t) {
            run.w.begin_object();
            run.w.member("op", std::string(op_names[t]));
            write_hist(r.h[t]);
            run.w.end_object();
        }
        run.w.end_array();
        run.w.key("synthetic");
        run.w.begin_array();
        for (int c = 0; c < cases; ++c) {
            run.w.begin_object();
            run.w.member("case", std::string(case_names[c]));
            write_hist(b.s.h[c]);
            run.w.end_object();
        }
        run.w.end_array();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "book_bench");
}
