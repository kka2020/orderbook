// Compile-time spec for include/lob/concepts.h. Every check here is a static_assert, so a
// violation fails the build rather than a test run. Each "Bad..." type breaks exactly one
// requirement, so each negative check pins down one rule of the concept.
#include <cstdint>
#include <optional>

#include "book_test_support.h"
#include "lob/baseline.h"
#include "lob/concepts.h"

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

        static_assert(!OrderBookLike<BadNoApply>);
        static_assert(!OrderBookLike<BadNoDepthAt>);
        static_assert(!OrderBookLike<BadNonConstQuery>);
        static_assert(!OrderBookLike<BadThrowingQuery>);
        static_assert(!OrderBookLike<BadBestBidType>);
        static_assert(!OrderBookLike<BadDepthType>);

        // ---- EventListener: on(const E&) for every event the book can emit ----

        struct GoodListener {
            void on(const OrderAck&) {}
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
            void on(const Trade&) {}
            void on(const CancelAck&) {}
            void on(const CancelReject&) {}
            void on(const ModifyAck&) {}
        };

        static_assert(EventListener<GoodListener>);
        static_assert(EventListener<GenericListener>);
        static_assert(EventListener<RecordingListener>);

        static_assert(!EventListener<BadNoModifyReject>);
        static_assert(!EventListener<int>);
    }
}
