#ifndef ORDERBOOK_BENCH_WORKLOAD_H
#define ORDERBOOK_BENCH_WORKLOAD_H
// Pre-generated benchmark workload: a deterministic stream of lob::Commands shaped like a real
// equity feed. Most orders are passive limits placed a few ticks from the touch, and most of them
// are cancelled rather than filled. Generation is untimed; the benchmark only replays the vectors.
//
// To know which orders are still resting, the generator feeds every command it emits through a
// private v1 BaselineBook (verified VALID against flash1) and tracks fills from its Trade events.
// So every cancel and modify targets a live order: no stale rejects distort the cancel timings.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "lob/baseline.h"
#include "lob/commands.h"
#include "lob/events.h"
#include "lob/types.h"

namespace lob::bench {

    // splitmix64: tiny, fast, and identical on every platform and standard library
    // (std::uniform_int_distribution is not, so the workload would differ between toolchains).
    class Rng {
    public:
        explicit Rng(std::uint64_t seed) noexcept : state_{seed} {}

        std::uint64_t next() noexcept {
            std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            return z ^ (z >> 31);
        }

        // Uniform in [0, n). The modulo bias is below 1e-15 for the small n used here.
        std::uint32_t below(std::uint32_t n) noexcept { return static_cast<std::uint32_t>(next() % n); }

        // True with probability percent / 100.
        bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }

    private:
        std::uint64_t state_;
    };

    struct WorkloadConfig {
        std::uint64_t seed = 42;
        std::size_t messages = 1'000'000;    // timed messages
        std::size_t target_depth = 5'000;    // resting orders the book hovers around
        std::int64_t start_mid = 100'000;    // ticks
        std::uint32_t new_percent = 50;      // message mix at the target depth; the rest are
        std::uint32_t modify_percent = 5;    // cancels (so 45% cancels by default)
        std::uint32_t aggressive_percent = 3; // new orders priced through the touch (they trade)
        std::uint32_t ioc_percent = 3;       // new orders that are IOC
    };

    struct Workload {
        std::vector<Command> prefill;  // untimed: GTC adds that build the book to target_depth
        std::vector<Command> timed;    // the measured stream

        // How resting (GTC) orders ended, prefill included: real equity feeds see ~95% cancelled.
        std::size_t rested = 0;     // GTC orders that rested at least briefly
        std::size_t filled = 0;     // ...and later fully filled
        std::size_t cancelled = 0;  // ...and later cancelled (the rest are still resting at the end)
    };

    namespace detail {
        struct LiveOrder {
            OrderId id;
            Side side;
            Price price;
            Quantity qty;  // open quantity, kept in step with the shadow book's fills
        };

        class Generator;

        // Listener for the generator's shadow book: only fills matter.
        struct FillTracker {
            Generator* gen;
            void on(const Trade& t) noexcept;
            void on(const OrderAck&) noexcept {}
            void on(const CancelAck&) noexcept {}
            void on(const CancelReject&) noexcept {}
            void on(const ModifyAck&) noexcept {}
            void on(const ModifyReject&) noexcept {}
        };

        class Generator {
        public:
            explicit Generator(const WorkloadConfig& c)
                : cfg_{c}, rng_{c.seed}, mid_{c.start_mid}, tracker_{this}, shadow_{tracker_} {
                live_.reserve(c.target_depth * 2);
                pos_.reserve(c.target_depth * 2);
            }

            // Reduce an order's open quantity after a fill; forget it once fully filled.
            // Ids not in pos_ (IOC takers) are ignored.
            void filled(OrderId id, Quantity q) noexcept {
                const auto it = pos_.find(id.value);
                if (it == pos_.end()) {
                    return;
                }
                LiveOrder& o = live_[it->second];
                o.qty -= q;
                if (o.qty.value == 0) {
                    remove_at(it->second);
                    ++filled_;
                }
            }

            Workload make() {
                Workload w;
                w.prefill.reserve(cfg_.target_depth);
                while (live_.size() < cfg_.target_depth) {
                    w.prefill.push_back(emit(new_order(/*passive_only=*/true)));
                }

                w.timed.reserve(cfg_.messages);
                for (std::size_t i = 0; i < cfg_.messages; ++i) {
                    // The mid drifts one tick every ~1,000 messages, so the touch moves and old
                    // orders end up further from it, as in a real session.
                    if (rng_.below(1000) == 0) {
                        mid_ += rng_.chance(50) ? 1 : -1;
                    }
                    w.timed.push_back(emit(next_message()));
                }
                w.rested = rested_;
                w.filled = filled_;
                w.cancelled = cancelled_;
                return w;
            }

        private:
            // Every command goes through the shadow book, whose Trades update live_ via filled().
            Command emit(const Command& c) {
                shadow_.apply(c);
                return c;
            }

            void add_live(const LiveOrder& o) {
                ++rested_;
                pos_[o.id.value] = live_.size();
                live_.push_back(o);
            }

            // O(1) removal: move the last order into the gap (order within live_ doesn't matter).
            void remove_at(std::size_t i) noexcept {
                pos_.erase(live_[i].id.value);
                if (i != live_.size() - 1) {
                    live_[i] = live_.back();
                    pos_[live_[i].id.value] = i;
                }
                live_.pop_back();
            }

            Command next_message() {
                if (live_.empty()) {
                    return new_order(false);
                }
                // Nudge the new-order share up or down by 5 points to hold the depth near target.
                const std::uint32_t new_pct =
                    live_.size() < cfg_.target_depth ? cfg_.new_percent + 5 : cfg_.new_percent - 5;
                const std::uint32_t roll = rng_.below(100);
                if (roll < new_pct) {
                    return new_order(false);
                }
                if (roll < new_pct + cfg_.modify_percent) {
                    return modify();
                }
                return cancel();
            }

            // Distance from the touch: 0 with probability 1/2, 1 with 1/4, ... capped at 30 ticks,
            // so resting liquidity clusters near the best prices.
            std::int64_t touch_distance() noexcept {
                std::int64_t d = 0;
                while (d < 30 && rng_.chance(50)) {
                    ++d;
                }
                return d;
            }

            Command new_order(bool passive_only) {
                const Side side = rng_.chance(50) ? Side::Buy : Side::Sell;
                const bool aggressive = !passive_only && rng_.chance(cfg_.aggressive_percent);
                const bool ioc = !passive_only && rng_.chance(cfg_.ioc_percent);
                const std::int64_t d = touch_distance();

                // Passive: at or behind our side of the mid, and never through the book's actual
                // opposite best (the mid drifts, so stale orders can sit beyond it). Aggressive:
                // through the other side, so it trades.
                std::int64_t px = 0;
                if (side == Side::Buy) {
                    px = aggressive ? mid_ + 1 + d : std::min(mid_ - 1 - d, ask_or(mid_ + 1) - 1);
                } else {
                    px = aggressive ? mid_ - 1 - d : std::max(mid_ + 1 + d, bid_or(mid_ - 1) + 1);
                }

                const NewOrder o{
                    .side = side,
                    .tif = ioc ? TimeInForce::IOC : TimeInForce::GTC,
                    .qty = Quantity{1 + rng_.below(100)},
                    .seq = next_seq(),
                    .id = OrderId{next_id_++},
                    .price = Price{px},
                };
                if (!ioc) {
                    add_live({o.id, o.side, o.price, o.qty});  // before emit(): it may fill at once
                }
                return o;
            }

            Command cancel() {
                const std::size_t i = rng_.below(static_cast<std::uint32_t>(live_.size()));
                const OrderId id = live_[i].id;
                remove_at(i);
                ++cancelled_;
                return Cancel{.seq = next_seq(), .id = id};
            }

            // Reprice by one tick (either way) and set a new quantity; keeps the order live.
            Command modify() {
                LiveOrder& o = live_[rng_.below(static_cast<std::uint32_t>(live_.size()))];
                // One tick either way, but away from the touch if moving toward it would cross:
                // a modify should reprice a quote, not turn it into an aggressive order.
                std::int64_t px = o.price.value + (rng_.chance(50) ? 1 : -1);
                if (o.side == Side::Buy && px >= ask_or(px + 1)) {
                    px = o.price.value - 1;
                } else if (o.side == Side::Sell && px <= bid_or(px - 1)) {
                    px = o.price.value + 1;
                }
                o.price = Price{px};
                o.qty = Quantity{1 + rng_.below(100)};
                return Modify{
                    .side = o.side,
                    .new_quantity = o.qty,
                    .seq = next_seq(),
                    .id = o.id,
                    .new_price = o.price,
                };
            }

            SeqNo next_seq() noexcept { return SeqNo{next_seq_++}; }

            std::int64_t ask_or(std::int64_t fallback) const noexcept {
                const auto a = shadow_.best_ask();
                return a ? a->value : fallback;
            }

            std::int64_t bid_or(std::int64_t fallback) const noexcept {
                const auto b = shadow_.best_bid();
                return b ? b->value : fallback;
            }

            WorkloadConfig cfg_;
            Rng rng_;
            std::int64_t mid_;
            std::vector<LiveOrder> live_;
            std::unordered_map<std::uint64_t, std::size_t> pos_;  // order id -> index in live_
            std::uint64_t next_id_ = 1;
            std::uint64_t next_seq_ = 1;
            std::size_t rested_ = 0, filled_ = 0, cancelled_ = 0;
            FillTracker tracker_;
            BaselineBook<FillTracker> shadow_;  // declared after tracker_, which it references
        };

        inline void FillTracker::on(const Trade& t) noexcept {
            gen->filled(t.maker_id, t.qty);
            gen->filled(t.taker_id, t.qty);
        }
    } // namespace detail

    inline Workload make_workload(const WorkloadConfig& cfg) {
        return detail::Generator{cfg}.make();
    }

} // namespace lob::bench

#endif // ORDERBOOK_BENCH_WORKLOAD_H
