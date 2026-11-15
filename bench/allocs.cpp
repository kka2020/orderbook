// Heap allocations per message, for every book: how many times operator new runs while the
// timed messages are replayed (workload generation and the prefill are excluded).
//
//   lob_allocs [messages]          default 1,000,000 (same workload as lob_latency)
//
// This is a separate program from lob_latency on purpose: replacing the global operator new
// affects everything in the program, and the latency and cachegrind numbers must not include the
// counter. The count is deterministic: same workload, same number on every run and machine
// (given the same standard library).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <variant>

#include "lob/baseline.h"
#include "lob/concepts.h"
#include "lob/fast_book.h"
#include "workload.h"

namespace {
    std::uint64_t g_allocations = 0;  // single-threaded program: a plain counter is enough
}

// Replacements for the global allocation functions. new[] and the sized/array deletes default to
// these, so every allocation by a standard container passes through here.
void* operator new(std::size_t n) {
    ++g_allocations;
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {
    using lob::bench::Workload;

    // Discards every event: the listener must not allocate, or it would be counted too.
    struct NullListener {
        template <typename E>
        void on(const E&) noexcept {}
    };
    static_assert(lob::EventListener<NullListener>);

    constexpr const char* kind_name[] = {"new", "cancel", "modify"};
    static_assert(std::variant_size_v<lob::Command> == std::size(kind_name));

    template <lob::OrderBookLike Book>
    void report(const char* name, const Workload& w) {
        NullListener listener;
        Book book {lob::bench::make_book<Book>(listener, w)};
        for (const lob::Command& c : w.prefill) {
            book.apply(c);
        }

        std::uint64_t allocs[std::size(kind_name)] {};
        std::uint64_t count[std::size(kind_name)] {};
        for (const lob::Command& c : w.timed) {
            const std::uint64_t before = g_allocations;
            book.apply(c);
            allocs[c.index()] += g_allocations - before;
            ++count[c.index()];
        }

        std::uint64_t total = 0;
        std::printf("%-16s", name);
        for (std::size_t k = 0; k < std::size(kind_name); ++k) {
            total += allocs[k];
            std::printf(" | %6.3f", count[k] ? static_cast<double>(allocs[k]) / static_cast<double>(count[k]) : 0.0);
        }
        std::printf(" | %6.3f |\n", static_cast<double>(total) / static_cast<double>(w.timed.size()));
    }
}

int main(int argc, char** argv) {
    lob::bench::WorkloadConfig cfg;
    if (argc >= 2) {
        cfg.messages = std::strtoull(argv[1], nullptr, 10);
        if (cfg.messages == 0) {
            std::fprintf(stderr, "usage: lob_allocs [messages > 0]\n");
            return 2;
        }
    }
    const Workload w = lob::bench::make_workload(cfg);

    std::printf("Heap allocations per message during the timed replay (%zu messages, prefill excluded)\n\n",
                w.timed.size());
    std::printf("%-16s | %6s | %6s | %6s | %6s |\n", "book", "new", "cancel", "modify", "all");
    report<lob::BaselineBook<NullListener>>("v1 BaselineBook", w);
    report<lob::FastBook<NullListener>>("v2 FastBook", w);
    return 0;
}
