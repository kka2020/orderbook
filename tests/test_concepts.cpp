// Compile-time spec for include/lob/concepts.h. Every check here is a static_assert, so a
// violation fails the build rather than a test run. Each "Bad..." type breaks exactly one
// requirement, so each negative check pins down one rule of the concept.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <optional>
#include <type_traits>
#include <vector>

#include "book_test_support.h"
#include "lob/baseline.h"
#include "lob/concepts.h"
#include "lob/fast_book.h"
#include "lob/policies.h"

namespace lob::test {
    namespace {
        // ---- OrderBookLike: apply(const Command&), plus const noexcept queries with exact return types ----

        struct GoodBook {
            void apply(const Command&) {}
            [[nodiscard]] std::optional<Price> best_bid() const noexcept { return std::nullopt; }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return std::nullopt; }
            [[nodiscard]] std::uint64_t depth_at(Price, Side) const noexcept { return 0; }
        };

        struct BadNoApply {
            [[nodiscard]] std::optional<Price> best_bid() const noexcept { return std::nullopt; }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return std::nullopt; }
            [[nodiscard]] std::uint64_t depth_at(Price, Side) const noexcept { return 0; }
        };

        struct BadNoDepthAt {
            void apply(const Command&) {}
            [[nodiscard]] std::optional<Price> best_bid() const noexcept { return std::nullopt; }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return std::nullopt; }
        };

        // Queries must be callable on a const book
        struct BadNonConstQuery {
            void apply(const Command&) {}
            [[nodiscard]] std::optional<Price> best_bid() noexcept { return std::nullopt; }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return std::nullopt; }
            [[nodiscard]] std::uint64_t depth_at(Price, Side) const noexcept { return 0; }
        };

        // Queries must be noexcept
        struct BadThrowingQuery {
            void apply(const Command&) {}
            [[nodiscard]] std::optional<Price> best_bid() const noexcept { return std::nullopt; }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return std::nullopt; }
            [[nodiscard]] std::uint64_t depth_at(Price, Side) const { return 0; }
        };

        // Return types must match exactly: a bare Price (flash1-style sentinel) is not allowed
        struct BadBestBidType {
            void apply(const Command&) {}
            [[nodiscard]] Price best_bid() const noexcept { return Price{0}; }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return std::nullopt; }
            [[nodiscard]] std::uint64_t depth_at(Price, Side) const noexcept { return 0; }
        };

        // Exact, not merely convertible: uint32_t would silently truncate deep levels
        struct BadDepthType {
            void apply(const Command&) {}
            [[nodiscard]] std::optional<Price> best_bid() const noexcept { return std::nullopt; }
            [[nodiscard]] std::optional<Price> best_ask() const noexcept { return std::nullopt; }
            [[nodiscard]] std::uint32_t depth_at(Price, Side) const noexcept { return 0; }
        };

        static_assert(OrderBookLike<GoodBook>);
        static_assert(OrderBookLike<BaselineBook<RecordingListener>>);
        static_assert(OrderBookLike<FastBook<RecordingListener>>);

        static_assert(!OrderBookLike<BadNoApply>);
        static_assert(!OrderBookLike<BadNoDepthAt>);
        static_assert(!OrderBookLike<BadNonConstQuery>);
        static_assert(!OrderBookLike<BadThrowingQuery>);
        static_assert(!OrderBookLike<BadBestBidType>);
        static_assert(!OrderBookLike<BadDepthType>);

        // ---- EventListener: on(const E&) for every event the book can emit ----

        struct GoodListener {
            void on(const OrderAck&) {}
            void on(const OrderReject&) {}
            void on(const Trade&) {}
            void on(const CancelAck&) {}
            void on(const CancelReject&) {}
            void on(const ModifyAck&) {}
            void on(const ModifyReject&) {}
        };

        // One generic handler is fine too
        struct GenericListener {
            template <typename E>
            void on(const E&) {}
        };

        // Missing one event type
        struct BadNoModifyReject {
            void on(const OrderAck&) {}
            void on(const OrderReject&) {}
            void on(const Trade&) {}
            void on(const CancelAck&) {}
            void on(const CancelReject&) {}
            void on(const ModifyAck&) {}
        };

        // Missing the new-order reject: a book could reject an order and have nowhere to report it
        struct BadNoOrderReject {
            void on(const OrderAck&) {}
            void on(const Trade&) {}
            void on(const CancelAck&) {}
            void on(const CancelReject&) {}
            void on(const ModifyAck&) {}
            void on(const ModifyReject&) {}
        };

        static_assert(EventListener<GoodListener>);
        static_assert(EventListener<GenericListener>);
        static_assert(EventListener<RecordingListener>);

        static_assert(!EventListener<BadNoModifyReject>);
        static_assert(!EventListener<BadNoOrderReject>);
        static_assert(!EventListener<int>);
    }
}

// ---- FastBook ownership rules (see the plan: move-only, rule of zero, FIFO by default) ----

namespace lob::test {
    namespace {
        using V2 = FastBook<RecordingListener>;

        // FIFO is the default matching policy.
        static_assert(std::is_same_v<V2, FastBook<RecordingListener, FifoPolicy>>);

        // Copying a book is almost always a bug (two books, one set of orders, one listener).
        static_assert(!std::is_copy_constructible_v<V2>);
        static_assert(!std::is_copy_assignable_v<V2>);

        // Moving is allowed and cannot throw, so a book can live in a std::vector or optional and
        // be handed between owners. (Move-ASSIGNMENT is not required: the book holds a Listener&,
        // and a reference cannot be re-pointed.)
        static_assert(std::is_nothrow_move_constructible_v<V2>);

        // The capacity hint is part of the constructor (default exists, so V2 book{listener} works).
        static_assert(std::is_constructible_v<V2, RecordingListener&, std::size_t>);
        static_assert(std::is_constructible_v<V2, RecordingListener&>);
    }
}

// ---- FifoLevel and MatchPolicyFor: the contract between the book and its matching policy ----
//
// FifoLevel<L>: what the FIFO policy needs from one price level's queue of orders:
//   - level.empty()      returns exactly bool;
//   - level.front().qty  is a modifiable Quantity (the policy reduces the front order's qty);
//   - level.pop_front()  removes the front order.
// MatchPolicyFor<P, Level>: P has a STATIC fill_level(Level&, Quantity, on_fill) returning exactly
//   Quantity (what is left of the incoming order after this level).
//
// These check shape only. What the policy does (oldest order first, qty reduced before on_fill,
// pop only at zero) is checked by the rule tests and the differential test.

namespace lob::test {
    namespace {
        // A stand-in for the book's private Order: the policy may only rely on .qty.
        struct FakeOrder {
            OrderId id {0};
            Quantity qty {0};
        };

        // ---- FifoLevel ----

        // Standard queues that have front/pop_front qualify, so step 0 (std::list) satisfies it.
        static_assert(FifoLevel<std::list<FakeOrder>>);
        static_assert(FifoLevel<std::deque<FakeOrder>>);

        // A hand-written level, the shape step 3's intrusive list will have.
        struct GoodLevel {
            [[nodiscard]] bool empty() const { return true; }
            FakeOrder& front() { return order; }
            void pop_front() {}
            FakeOrder order {};
        };
        static_assert(FifoLevel<GoodLevel>);

        // No pop_front: a vector can only remove from the back cheaply.
        static_assert(!FifoLevel<std::vector<FakeOrder>>);

        // empty() must be exactly bool, not something convertible to it.
        struct BadEmptyReturnsInt {
            [[nodiscard]] int empty() const { return 1; }
            FakeOrder& front() { return order; }
            void pop_front() {}
            FakeOrder order {};
        };
        static_assert(!FifoLevel<BadEmptyReturnsInt>);

        // front() returns a copy: reducing its qty would change nothing in the book.
        struct BadFrontReturnsCopy {
            [[nodiscard]] bool empty() const { return true; }
            [[nodiscard]] FakeOrder front() const { return {}; }
            void pop_front() {}
        };
        static_assert(!FifoLevel<BadFrontReturnsCopy>);

        // front() is read-only: the policy could not reduce the order's qty.
        struct BadFrontIsConst {
            [[nodiscard]] bool empty() const { return true; }
            [[nodiscard]] const FakeOrder& front() const { return order; }
            void pop_front() {}
            FakeOrder order {};
        };
        static_assert(!FifoLevel<BadFrontIsConst>);

        // The front order's qty is a raw integer, not a Quantity.
        struct RawQtyOrder {
            OrderId id;
            std::uint32_t qty;
        };
        static_assert(!FifoLevel<std::list<RawQtyOrder>>);

        // No qty at all.
        struct NoQtyOrder {
            OrderId id;
        };
        static_assert(!FifoLevel<std::list<NoQtyOrder>>);

        // ---- MatchPolicyFor ----

        using Level = std::list<FakeOrder>;

        // The real policy works on a real FIFO level...
        static_assert(MatchPolicyFor<FifoPolicy, Level>);
        static_assert(MatchPolicyFor<FifoPolicy, GoodLevel>);

        // ...and REFUSES a level without the FIFO shape. This only holds if fill_level's Level
        // parameter is constrained with FifoLevel; unconstrained, the misuse would compile here
        // and fail later, deep inside fill_level's body.
        static_assert(!MatchPolicyFor<FifoPolicy, std::vector<FakeOrder>>);

        // Any type with the right static fill_level qualifies, not just FifoPolicy.
        struct GoodPolicy {
            template <typename L, typename OnFill>
            static Quantity fill_level(L&, Quantity qty, OnFill&&) { return qty; }
        };
        static_assert(MatchPolicyFor<GoodPolicy, Level>);

        struct BadNoFillLevel {};
        static_assert(!MatchPolicyFor<BadNoFillLevel, Level>);

        // Must report what is left of the incoming order.
        struct BadFillLevelReturnsVoid {
            template <typename L, typename OnFill>
            static void fill_level(L&, Quantity, OnFill&&) {}
        };
        static_assert(!MatchPolicyFor<BadFillLevelReturnsVoid, Level>);

        // Must be static: the book never creates a policy object (it is a compile-time choice).
        struct BadFillLevelNotStatic {
            template <typename L, typename OnFill>
            Quantity fill_level(L&, Quantity qty, OnFill&&) { return qty; }
        };
        static_assert(!MatchPolicyFor<BadFillLevelNotStatic, Level>);

        // Arguments in the wrong order.
        struct BadFillLevelArgOrder {
            template <typename L, typename OnFill>
            static Quantity fill_level(Quantity qty, L&, OnFill&&) { return qty; }
        };
        static_assert(!MatchPolicyFor<BadFillLevelArgOrder, Level>);
    }
}
