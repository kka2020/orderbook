// FastBook-only promises that v1 does not make: it can be moved, and the capacity passed to the
// constructor is a hint, not a limit. (Matching behaviour is covered for both books by
// test_rules.cpp and test_differential.cpp.)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "book_test_support.h"
#include "lob/fast_book.h"

namespace lob::test {
    namespace {
        using V2 = FastBook<RecordingListener>;

        // A book moved into a new object keeps its orders, keeps matching them, and keeps
        // reporting to the same listener.
        TEST(FastBookMove, MovedBookKeepsItsOrdersAndListener) {
            RecordingListener listener;
            V2 original {listener};
            original.apply(buy(OrderId{10}, Price{100}, Quantity{5}));
            original.apply(sell(OrderId{20}, Price{105}, Quantity{7}));
            listener.events.clear();

            V2 moved {std::move(original)};

            EXPECT_EQ(moved.best_bid(), Price{100});
            EXPECT_EQ(moved.best_ask(), Price{105});
            EXPECT_EQ(moved.depth_at(Price{100}, Side::Buy), 5u);

            // Trade against a resting order, then cancel the other: both must still be reachable
            // by price (matching) and by id (cancel) after the move.
            moved.apply(sell(OrderId{30}, Price{100}, Quantity{2}));
            moved.apply(cancel(OrderId{20}));

            const std::vector<Event> expected {
                OrderAck{.side = Side::Sell, .qty = Quantity{2}, .seq = SeqNo{1}, .id = OrderId{30}, .price = Price{100}},
                Trade{.qty = Quantity{2}, .seq = SeqNo{1}, .maker_id = OrderId{10}, .taker_id = OrderId{30}, .price = Price{100}},
                CancelAck{.side = Side::Sell, .qty = Quantity{7}, .seq = SeqNo{1}, .id = OrderId{20}, .price = Price{105}},
            };
            EXPECT_EQ(listener.events, expected);
            EXPECT_EQ(moved.depth_at(Price{100}, Side::Buy), 3u);
            EXPECT_EQ(moved.best_ask(), std::nullopt);
        }

        // A book can live in a std::vector: growing the vector moves the books it holds.
        TEST(FastBookMove, BooksSurviveVectorGrowth) {
            RecordingListener listener;
            std::vector<V2> books;
            for (std::size_t i = 0; i < 20; ++i) {  // several reallocations
                books.emplace_back(listener);
                books.back().apply(buy(OrderId{10 + i}, Price{static_cast<std::int64_t>(100 + i)}, Quantity{1}));
            }
            for (std::size_t i = 0; i < books.size(); ++i) {
                EXPECT_EQ(books[i].best_bid(), Price{static_cast<std::int64_t>(100 + i)}) << "book " << i;
            }
        }

        // Capacity is a hint: going past it must work (the book grows), not fail or corrupt orders.
        TEST(FastBookCapacity, MoreRestingOrdersThanCapacityStillWork) {
            RecordingListener listener;
            V2 book {listener, 4};
            constexpr std::size_t n = 1000;
            for (std::size_t i = 0; i < n; ++i) {
                // 10 price levels, 100 orders each, so both levels and orders outgrow the hint.
                book.apply(buy(OrderId{10 + i}, Price{static_cast<std::int64_t>(100 + i % 10)}, Quantity{1}));
            }
            EXPECT_EQ(book.best_bid(), Price{109});
            EXPECT_EQ(book.depth_at(Price{100}, Side::Buy), 100u);

            // Every order is still findable by id after all the growth.
            listener.events.clear();
            for (std::size_t i = 0; i < n; ++i) {
                book.apply(cancel(OrderId{10 + i}));
            }
            std::size_t acks = 0;
            for (const Event& e : listener.events) {
                if (std::holds_alternative<CancelAck>(e)) {
                    ++acks;
                }
            }
            EXPECT_EQ(acks, n) << "some cancels were rejected: orders were lost while the book grew";
            EXPECT_EQ(book.best_bid(), std::nullopt);
        }
    }
}
