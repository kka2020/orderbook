// Differential test: feed the same long command stream to a reference book and a candidate book,
// and after EVERY command check that both emitted identical events and agree on the book state
// (best bid, best ask, and depth at the prices that matter for that step).
//
// The reference is v1 BaselineBook, verified VALID against the flash1 benchmark. Candidates are
// listed in `Pairs` below: v2 FastBook at its default capacity and at a tiny one (to force growth).
// v1 vs v1 stays as a control, and the HarnessSelfTest cases prove the harness does catch a
// candidate that diverges.
//
// Two kinds of stream:
//   - adversarial: few ids and a narrow price band, so ids are reused, duplicated and cancelled
//     after filling, orders cross constantly, and every reject path is hit (zero qty, int64-extreme
//     prices, duplicate ids, wrong-side and stale modifies), across GTC, IOC and market orders;
//   - realistic: the benchmark workload (bench/workload.h): a deep book, mostly cancels.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "book_test_support.h"
#include "lob/baseline.h"
#include "lob/fast_book.h"
#include "workload.h"

namespace lob::test {
    namespace {

        // ---- streams ------------------------------------------------------------------------------

        constexpr std::int64_t adversarial_lo = 95, adversarial_hi = 105;

        std::vector<Command> adversarial_stream(std::uint64_t seed, std::size_t count) {
            bench::Rng rng {seed};
            constexpr std::int64_t lo = adversarial_lo, hi = adversarial_hi;  // narrow band: orders cross all the time
            constexpr std::uint32_t ids = 64;          // small id space: constant reuse and duplicates

            auto price = [&]() -> Price {
                if (rng.chance(1)) {  // the reserved extremes, to exercise InvalidPrice
                    return Price{rng.chance(50) ? std::numeric_limits<std::int64_t>::min()
                                                : std::numeric_limits<std::int64_t>::max()};
                }
                return Price{lo + static_cast<std::int64_t>(rng.below(static_cast<std::uint32_t>(hi - lo + 1)))};
            };
            auto qty = [&]() { return Quantity{rng.chance(3) ? 0u : 1 + rng.below(20)}; };
            auto id = [&]() { return OrderId{1 + rng.below(ids)}; };
            auto side = [&]() { return rng.chance(50) ? Side::Buy : Side::Sell; };

            std::vector<Command> out;
            out.reserve(count);
            for (std::uint64_t seq = 1; seq <= count; ++seq) {
                const std::uint32_t roll = rng.below(100);
                if (roll < 55) {
                    const std::uint32_t t = rng.below(100);
                    const TimeInForce tif = t < 70 ? TimeInForce::GTC : t < 90 ? TimeInForce::IOC : TimeInForce::Market;
                    out.push_back(NewOrder{.side = side(), .tif = tif, .qty = qty(), .seq = SeqNo{seq}, .id = id(), .price = price()});
                } else if (roll < 85) {
                    out.push_back(Cancel{.seq = SeqNo{seq}, .id = id()});
                } else {
                    // Random side: about half of the modifies of live orders name the wrong side.
                    out.push_back(Modify{.side = side(), .new_quantity = qty(), .seq = SeqNo{seq}, .id = id(), .new_price = price()});
                }
            }
            return out;
        }

        // Every valid price of the adversarial band, for a full-book depth sweep.
        std::vector<Price> adversarial_prices() {
            std::vector<Price> out;
            for (std::int64_t p = adversarial_lo; p <= adversarial_hi; ++p) {
                out.push_back(Price{p});
            }
            return out;
        }

        std::vector<Command> realistic_stream(std::uint64_t seed, std::size_t count) {
            bench::WorkloadConfig cfg;
            cfg.seed = seed;
            cfg.messages = count;
            bench::Workload w = bench::make_workload(cfg);
            std::vector<Command> out = std::move(w.prefill);
            out.insert(out.end(), w.timed.begin(), w.timed.end());
            return out;
        }

        // ---- the comparison -----------------------------------------------------------------------

        std::optional<Price> command_price(const Command& c) {
            if (const auto* n = std::get_if<NewOrder>(&c)) {
                return n->price;
            }
            if (const auto* m = std::get_if<Modify>(&c)) {
                return m->new_price;
            }
            return std::nullopt;
        }

        // Runs `stream` through both books. Returns a description of the first divergence, or
        // nullopt if they agree after every command. Events and best bid/ask are compared after
        // every command; depth only after every `depth_every`-th one, because v1 sums a level's queue
        // on demand and the realistic book has ~1,250 orders at each best level. A wrong depth
        // persists in the book state, so it is still caught at the next sampled command.
        //
        // Depth is probed at the best bid, best ask and the command's price, plus every price in
        // `sweep` (both sides). The sweep catches a stale total at a level deep in the book on the
        // command that corrupts it, not only once that level reaches the top or is traded.
        template <typename Ref, typename Cand>
        std::optional<std::string> first_divergence(const std::vector<Command>& stream, std::size_t depth_every = 1,
                                                    std::span<const Price> sweep = {}) {
            RecordingListener ref_events;
            RecordingListener cand_events;
            Ref ref {ref_events};
            Cand cand {cand_events};

            for (std::size_t i = 0; i < stream.size(); ++i) {
                const Command& c = stream[i];
                ref.apply(c);
                cand.apply(c);

                // Compare first; build a message only on a mismatch (constructing a stream for every
                // one of ~10^6 commands would dominate the run time).
                auto report = [&](const std::string& why) {
                    return "first divergence at command #" + std::to_string(i) + ": " + ::testing::PrintToString(c) +
                           "\n" + why;
                };
                if (cand_events.events != ref_events.events) {
                    return report("events differ\n  reference: " + ::testing::PrintToString(ref_events.events) +
                                  "\n  candidate: " + ::testing::PrintToString(cand_events.events));
                }
                if (cand.best_bid() != ref.best_bid() || cand.best_ask() != ref.best_ask()) {
                    return report("best bid/ask differ\n  reference: bid " + ::testing::PrintToString(ref.best_bid()) +
                                  " ask " + ::testing::PrintToString(ref.best_ask()) + "\n  candidate: bid " +
                                  ::testing::PrintToString(cand.best_bid()) + " ask " +
                                  ::testing::PrintToString(cand.best_ask()));
                }
                // Depth at the levels this step could have changed or exposed.
                if (i % depth_every != 0) {
                    ref_events.events.clear();
                    cand_events.events.clear();
                    continue;
                }
                auto depth_mismatch = [&](Price probe) -> std::optional<std::string> {
                    for (const Side s : {Side::Buy, Side::Sell}) {
                        const std::uint64_t want = ref.depth_at(probe, s);
                        const std::uint64_t got = cand.depth_at(probe, s);
                        if (got != want) {
                            return report("depth_at(" + std::to_string(probe.value) + ", " + ::testing::PrintToString(s) +
                                          ") differs: reference " + std::to_string(want) + ", candidate " +
                                          std::to_string(got));
                        }
                    }
                    return std::nullopt;
                };
                for (const std::optional<Price>& probe : {ref.best_bid(), ref.best_ask(), command_price(c)}) {
                    if (probe) {
                        if (auto m = depth_mismatch(*probe)) {
                            return m;
                        }
                    }
                }
                for (const Price probe : sweep) {
                    if (auto m = depth_mismatch(probe)) {
                        return m;
                    }
                }
                ref_events.events.clear();
                cand_events.events.clear();
            }
            return std::nullopt;
        }

        // ---- book pairs under test ----------------------------------------------------------------

        template <typename R, typename C>
        struct Pair {
            using Ref = R;
            using Cand = C;
        };

        template <typename P>
        class DifferentialTest : public ::testing::Test {};

        // Stream sizes: full in optimised builds (release; CI runs both presets), 1/10 in the debug
        // build so the edit-test loop under ASan/UBSan at -O0 stays quick.
#ifdef NDEBUG
        constexpr std::size_t stream_length = 200'000;
#else
        constexpr std::size_t stream_length = 20'000;
#endif

        using V1 = BaselineBook<RecordingListener>;
        using V2 = FastBook<RecordingListener>;

        // v2 with a tiny capacity hint, so every stream overflows it many times and the growth
        // path (pool / index / level storage resizing mid-run) is checked as hard as the normal one.
        struct TinyV2 : V2 {
            explicit TinyV2(RecordingListener& l) : V2{l, 8} {}
        };

        using Pairs = ::testing::Types<Pair<V1, V1>, Pair<V1, V2>, Pair<V1, TinyV2>>;
        TYPED_TEST_SUITE(DifferentialTest, Pairs);

        TYPED_TEST(DifferentialTest, AdversarialStreamsMatch) {
            using Ref = typename TypeParam::Ref;
            using Cand = typename TypeParam::Cand;
            const std::vector<Price> sweep = adversarial_prices();  // 11 prices: cheap in a tiny book
            for (std::uint64_t seed = 1; seed <= 5; ++seed) {
                const auto divergence = first_divergence<Ref, Cand>(adversarial_stream(seed, stream_length), 1, sweep);
                EXPECT_EQ(divergence, std::nullopt) << "adversarial seed " << seed;
            }
        }

        TYPED_TEST(DifferentialTest, RealisticStreamMatches) {
            using Ref = typename TypeParam::Ref;
            using Cand = typename TypeParam::Cand;
            const auto divergence = first_divergence<Ref, Cand>(realistic_stream(42, stream_length), 64);
            EXPECT_EQ(divergence, std::nullopt) << "realistic seed 42";
        }

        // The adversarial stream must actually reach the paths it claims to: count each event kind
        // across one seed, so a generator change that silently stops producing (say) rejects fails.
        TEST(DifferentialStreams, AdversarialStreamHitsEveryEventKind) {
            RecordingListener l;
            V1 book {l};
            for (const Command& c : adversarial_stream(1, 200'000)) {  // cheap: one book, no probes
                book.apply(c);
            }
            std::size_t counts[std::variant_size_v<Event>] {};
            std::size_t reasons[3] {};
            for (const Event& e : l.events) {
                ++counts[e.index()];
                if (const auto* r = std::get_if<OrderReject>(&e)) {
                    ++reasons[static_cast<std::size_t>(r->reason)];
                }
            }
            for (std::size_t k = 0; k < std::variant_size_v<Event>; ++k) {
                EXPECT_GT(counts[k], 100u) << "event kind #" << k << " of lob::test::Event is (almost) never emitted";
            }
            for (std::size_t r = 0; r < 3; ++r) {
                EXPECT_GT(reasons[r], 100u) << "RejectReason #" << r << " is (almost) never produced";
            }
        }

        // ---- the harness catches divergence -------------------------------------------------------

        // v1, but its listener sees one extra CancelReject on the 500th command.
        struct ExtraEventBook {
            RecordingListener& l;
            V1 inner;
            std::size_t applied = 0;
            explicit ExtraEventBook(RecordingListener& listener) : l{listener}, inner{listener} {}
            void apply(const Command& c) {
                inner.apply(c);
                if (++applied == 500) {
                    l.on(CancelReject{.seq = SeqNo{0}, .id = OrderId{0}});
                }
            }
            [[nodiscard]] std::optional<Price> best_bid() const noexcept { return inner.best_bid(); }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return inner.best_ask(); }
            [[nodiscard]] std::uint64_t depth_at(Price p, Side s) const noexcept { return inner.depth_at(p, s); }
        };

        // v1 with correct events, but depth_at over-reports one unit at the best bid from the 500th
        // command on: a state-only bug (like a stale cached level total) that no event reveals.
        struct WrongDepthBook {
            V1 inner;
            std::size_t applied = 0;
            explicit WrongDepthBook(RecordingListener& listener) : inner{listener} {}
            void apply(const Command& c) {
                inner.apply(c);
                ++applied;
            }
            [[nodiscard]] std::optional<Price> best_bid() const noexcept { return inner.best_bid(); }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return inner.best_ask(); }
            [[nodiscard]] std::uint64_t depth_at(Price p, Side s) const noexcept {
                const std::uint64_t d = inner.depth_at(p, s);
                const bool skewed = applied >= 500 && s == Side::Buy && inner.best_bid() == p;
                return skewed ? d + 1 : d;
            }
        };

        // v1 with correct events, but from the 500th command on depth_at over-reports one unit at
        // bid price 95 whenever 95 is NOT the best bid: a stale total deep in the book.
        struct WrongDeepDepthBook {
            V1 inner;
            std::size_t applied = 0;
            explicit WrongDeepDepthBook(RecordingListener& listener) : inner{listener} {}
            void apply(const Command& c) {
                inner.apply(c);
                ++applied;
            }
            [[nodiscard]] std::optional<Price> best_bid() const noexcept { return inner.best_bid(); }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return inner.best_ask(); }
            [[nodiscard]] std::uint64_t depth_at(Price p, Side s) const noexcept {
                const std::uint64_t d = inner.depth_at(p, s);
                const bool skewed = applied >= 500 && s == Side::Buy && p == Price{95} && inner.best_bid() != p;
                return skewed ? d + 1 : d;
            }
        };

        static_assert(OrderBookLike<ExtraEventBook>);
        static_assert(OrderBookLike<WrongDepthBook>);
        static_assert(OrderBookLike<WrongDeepDepthBook>);

        TEST(DifferentialHarnessSelfTest, CatchesAnExtraEvent) {
            const auto divergence = first_divergence<V1, ExtraEventBook>(adversarial_stream(1, 2'000));
            ASSERT_TRUE(divergence.has_value());
            EXPECT_NE(divergence->find("command #499"), std::string::npos) << *divergence;
            EXPECT_NE(divergence->find("events differ"), std::string::npos) << *divergence;
        }

        TEST(DifferentialHarnessSelfTest, CatchesWrongDepthWithIdenticalEvents) {
            const auto divergence = first_divergence<V1, WrongDepthBook>(realistic_stream(42, 2'000), 64);
            ASSERT_TRUE(divergence.has_value());
            EXPECT_NE(divergence->find("command #"), std::string::npos) << *divergence;
            EXPECT_NE(divergence->find("depth_at("), std::string::npos) << *divergence;
        }

        // With the sweep, a stale total deep in the book is caught on the very command it starts.
        TEST(DifferentialHarnessSelfTest, SweepCatchesWrongDepthDeepInTheBookImmediately) {
            const std::vector<Price> sweep = adversarial_prices();
            const auto divergence = first_divergence<V1, WrongDeepDepthBook>(adversarial_stream(1, 2'000), 1, sweep);
            ASSERT_TRUE(divergence.has_value());
            EXPECT_NE(divergence->find("command #499"), std::string::npos) << *divergence;
            EXPECT_NE(divergence->find("depth_at(95, Buy)"), std::string::npos) << *divergence;
        }
    }
}
