// Per-operation latency and throughput benchmark for every OrderBookLike book.
//
//   lob_latency [rounds] [messages]          default: 11 rounds, 1,000,000 messages
//   taskset -c 2 lob_latency                  pin to one CPU (best effort under WSL)
//   lob_latency --replay-once [messages]      one untimed replay, for cachegrind.sh
//
// Each round, for each book type in turn (so a slow phase of the machine hits every book alike):
//   1. latency pass:    fresh book, apply the untimed prefill, then time every timed message
//                       with rdtsc and record it by type (new / cancel / modify);
//   2. throughput pass: fresh book, prefill, then time the whole replay with no per-op timers.
// Reported per percentile: the median across rounds, with the min-max range showing the spread.
//
// Timing: lfence + rdtsc before the operation and rdtscp + lfence after it, so the measured
// window cannot start before earlier work finishes or end before the operation finishes. The cost
// of an empty start/stop pair is measured and subtracted. Cycles are converted to nanoseconds with
// a TSC frequency calibrated against std::chrono::steady_clock.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <iterator>
#include <type_traits>
#include <variant>
#include <vector>

#include <x86intrin.h>

// cachegrind.sh measures only between these markers. Under plain execution they are a few no-op
// instructions; without the valgrind headers (e.g. on CI) they compile to nothing.
#if __has_include(<valgrind/cachegrind.h>)
#include <valgrind/cachegrind.h>
#else
#define CACHEGRIND_START_INSTRUMENTATION
#define CACHEGRIND_STOP_INSTRUMENTATION
#endif

#include "lob/baseline.h"
#include "lob/concepts.h"
#include "workload.h"

namespace {

    using lob::bench::Workload;

    // Counts events and nothing else: no I/O or allocation inside the timed region. The counts
    // are printed at the end, which also stops the compiler from optimising the work away.
    struct CountingListener {
        std::uint64_t acks = 0, order_rejects = 0, trades = 0, cancel_acks = 0, cancel_rejects = 0;
        std::uint64_t modify_acks = 0, modify_rejects = 0;

        void on(const lob::OrderAck&) noexcept { ++acks; }
        void on(const lob::OrderReject&) noexcept { ++order_rejects; }
        void on(const lob::Trade&) noexcept { ++trades; }
        void on(const lob::CancelAck&) noexcept { ++cancel_acks; }
        void on(const lob::CancelReject&) noexcept { ++cancel_rejects; }
        void on(const lob::ModifyAck&) noexcept { ++modify_acks; }
        void on(const lob::ModifyReject&) noexcept { ++modify_rejects; }
    };

    // Every book under test. Adding v2 later: one more entry here, plus a name in book_name().
    using V1 = lob::BaselineBook<CountingListener>;

    template <typename Book>
    constexpr const char* book_name() {
        if constexpr (std::is_same_v<Book, V1>) {
            return "v1 BaselineBook";
        } else {
            return "?";
        }
    }

    // ---- timing ---------------------------------------------------------------------------------

    inline std::uint64_t tsc_start() noexcept {
        _mm_lfence();          // wait for earlier instructions before reading the counter
        return __rdtsc();
    }

    inline std::uint64_t tsc_stop() noexcept {
        unsigned aux = 0;
        const std::uint64_t t = __rdtscp(&aux);  // waits for the timed work to finish
        _mm_lfence();          // and stops later instructions starting before the read
        return t;
    }

    // Median cost (in TSC ticks) of an empty start/stop pair, subtracted from every sample.
    std::uint64_t timer_overhead() {
        std::vector<std::uint64_t> s(100'000);
        for (auto& x : s) {
            const std::uint64_t a = tsc_start();
            x = tsc_stop() - a;
        }
        std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2), s.end());
        return s[s.size() / 2];
    }

    // TSC ticks per nanosecond, by busy-waiting ~200 ms against steady_clock.
    double tsc_per_ns() {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        const std::uint64_t c0 = __rdtsc();
        while (clock::now() - t0 < std::chrono::milliseconds(200)) {
        }
        const std::uint64_t c1 = __rdtsc();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
        return static_cast<double>(c1 - c0) / static_cast<double>(ns);
    }

    // ---- one round ------------------------------------------------------------------------------

    enum Kind : std::size_t { New, Cancel, Modify, KindCount };

    // Samples are filed by cycles[command.index()], so each Kind must equal its type's position in
    // the lob::Command variant. Reordering or extending Command breaks the build here instead of
    // silently mislabelling the rows.
    static_assert(std::variant_size_v<lob::Command> == KindCount);
    static_assert(std::is_same_v<std::variant_alternative_t<New, lob::Command>, lob::NewOrder>);
    static_assert(std::is_same_v<std::variant_alternative_t<Cancel, lob::Command>, lob::Cancel>);
    static_assert(std::is_same_v<std::variant_alternative_t<Modify, lob::Command>, lob::Modify>);
    constexpr const char* kind_name[KindCount] = {"new", "cancel", "modify"};
    constexpr double percentiles[] = {0.50, 0.90, 0.99, 0.999};
    constexpr std::size_t pct_count = std::size(percentiles);

    struct RoundResult {
        double pct_ns[KindCount][pct_count]{};  // [kind][percentile]
        std::size_t samples[KindCount]{};
        double mmsgs_per_s = 0;                 // from the throughput pass
        CountingListener counts;                // from the latency pass
    };

    template <lob::OrderBookLike Book>
    void prefill(Book& book, const Workload& w) {
        for (const lob::Command& c : w.prefill) {
            book.apply(c);
        }
    }

    template <lob::OrderBookLike Book>
    RoundResult run_round(const Workload& w, std::uint64_t overhead, double ticks_per_ns) {
        RoundResult r;

        // Latency pass. Sample buffers are allocated AND written before timing: resize (not reserve)
        // zero-fills every element, so the OS maps all their memory pages now. With reserve, each
        // 4 KB page would be mapped by a page fault on its first write, mid-run, between timed ops.
        {
            std::vector<std::uint32_t> cycles[KindCount];
            std::size_t filled[KindCount]{};
            for (auto& v : cycles) {
                v.resize(w.timed.size());
            }
            CountingListener listener;
            Book book{listener};
            prefill(book, w);

            for (const lob::Command& c : w.timed) {
                const std::uint64_t a = tsc_start();
                book.apply(c);
                const std::uint64_t b = tsc_stop();
                const std::uint64_t d = b - a > overhead ? b - a - overhead : 0;
                const std::size_t k = c.index();
                cycles[k][filled[k]++] = static_cast<std::uint32_t>(std::min<std::uint64_t>(d, UINT32_MAX));
            }
            r.counts = listener;

            for (std::size_t k = 0; k < KindCount; ++k) {
                auto& v = cycles[k];
                v.resize(filled[k]);  // drop the unused tail before sorting
                r.samples[k] = v.size();
                if (v.empty()) {
                    continue;
                }
                std::sort(v.begin(), v.end());
                for (std::size_t p = 0; p < pct_count; ++p) {
                    const auto idx = static_cast<std::size_t>(percentiles[p] * static_cast<double>(v.size() - 1));
                    r.pct_ns[k][p] = static_cast<double>(v[idx]) / ticks_per_ns;
                }
            }
        }

        // Throughput pass: the whole replay under one timer, no per-op overhead.
        {
            CountingListener listener;
            Book book{listener};
            prefill(book, w);
            const std::uint64_t a = tsc_start();
            for (const lob::Command& c : w.timed) {
                book.apply(c);
            }
            const std::uint64_t b = tsc_stop();
            const double ns = static_cast<double>(b - a) / ticks_per_ns;
            r.mmsgs_per_s = static_cast<double>(w.timed.size()) / ns * 1e3;
            if (listener.acks == 0) {
                std::puts("unreachable: keeps the throughput pass observable");
            }
        }
        return r;
    }

    // ---- reporting ------------------------------------------------------------------------------

    struct Stat {
        double median, lo, hi;
    };

    Stat stat_of(std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return {v[v.size() / 2], v.front(), v.back()};
    }

    void print_mix(const RoundResult& r, const Workload& w) {
        const std::size_t messages = w.timed.size();
        const auto& c = r.counts;
        auto pct = [&](std::size_t n) { return 100.0 * static_cast<double>(n) / static_cast<double>(messages); };
        std::printf("Workload: %zu timed messages: %.1f%% new, %.1f%% cancel, %.1f%% modify\n", messages,
                    pct(r.samples[New]), pct(r.samples[Cancel]), pct(r.samples[Modify]));
        std::printf("Outcome:  %llu order rejects, %llu trades, %llu cancel acks (incl. IOC remainders), %llu cancel rejects, "
                    "%llu modify acks, %llu modify rejects\n",
                    static_cast<unsigned long long>(c.order_rejects),
                    static_cast<unsigned long long>(c.trades), static_cast<unsigned long long>(c.cancel_acks),
                    static_cast<unsigned long long>(c.cancel_rejects), static_cast<unsigned long long>(c.modify_acks),
                    static_cast<unsigned long long>(c.modify_rejects));
        std::printf("Lifecycle: of %zu resting orders, %.1f%% ended cancelled, %.1f%% fully filled, "
                    "%.1f%% still resting\n\n",
                    w.rested, 100.0 * static_cast<double>(w.cancelled) / static_cast<double>(w.rested),
                    100.0 * static_cast<double>(w.filled) / static_cast<double>(w.rested),
                    100.0 * static_cast<double>(w.rested - w.cancelled - w.filled) / static_cast<double>(w.rested));
    }

    template <typename Book>
    void print_book(const std::vector<RoundResult>& rounds) {
        std::printf("### %s (%zu rounds)\n\n", book_name<Book>(), rounds.size());
        std::printf("| op | p50 ns | p90 ns | p99 ns | p99.9 ns |\n|---|---:|---:|---:|---:|\n");
        for (std::size_t k = 0; k < KindCount; ++k) {
            std::printf("| %s |", kind_name[k]);
            for (std::size_t p = 0; p < pct_count; ++p) {
                std::vector<double> v;
                for (const auto& r : rounds) {
                    v.push_back(r.pct_ns[k][p]);
                }
                const Stat s = stat_of(v);
                std::printf(" %.0f (%.0f-%.0f) |", s.median, s.lo, s.hi);
            }
            std::printf("\n");
        }
        std::vector<double> t;
        for (const auto& r : rounds) {
            t.push_back(r.mmsgs_per_s);
        }
        const Stat s = stat_of(t);
        std::printf("\nThroughput: %.2f M msgs/s (range %.2f-%.2f)\n\n", s.median, s.lo, s.hi);
    }

    // ---- cachegrind mode ------------------------------------------------------------------------

    // cachegrind.sh starts valgrind with instrumentation off, so only this replay is counted:
    // workload generation and the prefill are excluded.
    template <lob::OrderBookLike Book>
    void replay_once(Book& book, const Workload& w) {
        CACHEGRIND_START_INSTRUMENTATION;
        for (const lob::Command& c : w.timed) {
            book.apply(c);
        }
        CACHEGRIND_STOP_INSTRUMENTATION;
    }

    template <lob::OrderBookLike Book>
    void run_replay_once(const Workload& w) {
        CountingListener listener;
        Book book{listener};
        prefill(book, w);
        replay_once(book, w);
        std::printf("%s: replayed %zu messages (%llu trades)\n", book_name<Book>(), w.timed.size(),
                    static_cast<unsigned long long>(listener.trades));
    }

    std::size_t parse_count(const char* s) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(s, &end, 10);
        if (end == s || *end != '\0' || v == 0) {
            std::fprintf(stderr, "expected a positive integer, got '%s'\n", s);
            std::exit(2);
        }
        return static_cast<std::size_t>(v);
    }

} // namespace

int main(int argc, char** argv) {
    lob::bench::WorkloadConfig cfg;

    if (argc >= 2 && std::string_view{argv[1]} == "--replay-once") {
        if (argc >= 3) {
            cfg.messages = parse_count(argv[2]);
        }
        const Workload w = lob::bench::make_workload(cfg);
        run_replay_once<V1>(w);
        return 0;
    }

    const std::size_t rounds = argc >= 2 ? parse_count(argv[1]) : 11;
    if (argc >= 3) {
        cfg.messages = parse_count(argv[2]);
    }

    const Workload w = lob::bench::make_workload(cfg);
    const std::uint64_t overhead = timer_overhead();
    const double ticks_per_ns = tsc_per_ns();
    std::printf("TSC %.3f GHz, timer overhead %llu TSC ticks (subtracted). Prefill %zu orders, seed %llu.\n",
                ticks_per_ns, static_cast<unsigned long long>(overhead), w.prefill.size(),
                static_cast<unsigned long long>(cfg.seed));

    std::vector<RoundResult> v1;
    for (std::size_t i = 0; i < rounds; ++i) {
        v1.push_back(run_round<V1>(w, overhead, ticks_per_ns));
        // v2: interleave here, e.g. v2.push_back(run_round<V2>(w, overhead, ticks_per_ns));
    }

    print_mix(v1.front(), w);
    print_book<V1>(v1);
    return 0;
}
