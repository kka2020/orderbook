// FastBook-only promises that v1 does not make: it can be moved, the capacity passed to the
// constructor is a hint, not a limit (the order pool grows past it, and queues survive the
// growth), and the id table's own entries stay correct while matching rearranges it. (Matching
// behaviour is covered for both books by test_rules.cpp and test_differential.cpp.)

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

        // Growing the pool moves every order to new memory. A queue linked by slot numbers survives
        // that; one linked by pointers or references into the pool would not (ASan reports it in
        // the debug build). Capacity 1 makes the pool grow on almost every add.
        TEST(FastBookCapacity, QueueOrderSurvivesPoolGrowth) {
            RecordingListener listener;
            V2 book {listener, 1};
            constexpr std::size_t n = 300;
            for (std::size_t i = 0; i < n; ++i) {
                book.apply(sell(OrderId{10 + i}, Price{100}, Quantity{1}));
            }
            // Unlink every third order while the pool is at its largest...
            for (std::size_t i = 0; i < n; i += 3) {
                book.apply(cancel(OrderId{10 + i}));
            }
            // ...then grow it again with orders that land on the back of the same queue.
            for (std::size_t i = n; i < 2 * n; ++i) {
                book.apply(sell(OrderId{10 + i}, Price{100}, Quantity{1}));
            }

            std::vector<OrderId> expected;
            for (std::size_t i = 0; i < 2 * n; ++i) {
                if (i >= n || i % 3 != 0) {
                    expected.push_back(OrderId{10 + i});
                }
            }
            EXPECT_EQ(book.depth_at(Price{100}, Side::Sell), expected.size());

            listener.events.clear();
            book.apply(market_buy(OrderId{100000}, Quantity{100000}));
            std::vector<OrderId> makers;
            for (const Event& e : listener.events) {
                if (const auto* t = std::get_if<Trade>(&e)) {
                    makers.push_back(t->maker_id);
                }
            }
            EXPECT_EQ(makers, expected);
            EXPECT_EQ(book.best_ask(), std::nullopt);
        }

        // A small id table (capacity 1) is crowded, so erasing an id often shifts other entries
        // back to close the gap (backward-shift deletion). Each round, a taker fully fills 100
        // makers, so 100 ids are erased while it matches, then rests its remainder. From v2 step 6
        // the taker's id is claimed before matching: its entry must not be written through a
        // pointer or reference taken before those erases, which may have moved it.
        TEST(FastBookIdTable, ATakersEntrySurvivesTheErasesMadeWhileItMatches) {
            RecordingListener listener;
            V2 book {listener, 1};
            std::uint64_t next_id {1};
            for (std::int64_t round = 0; round < 50; ++round) {
                SCOPED_TRACE(testing::Message() << "round " << round);
                const Price price {1'000 + round};
                for (int k = 0; k < 100; ++k) {
                    book.apply(sell(OrderId{next_id++}, price, Quantity{1}));
                }
                const OrderId taker {next_id++};
                book.apply(buy(taker, price, Quantity{103}));  // fills 100, rests 3
                ASSERT_EQ(book.best_bid(), price);

                listener.events.clear();
                book.apply(cancel(taker));
                const std::vector<Event> expected {
                    CancelAck{.side = Side::Buy, .qty = Quantity{3}, .seq = SeqNo{1}, .id = taker, .price = price},
                };
                ASSERT_EQ(listener.events, expected);
                ASSERT_EQ(book.best_bid(), std::nullopt);
            }
        }
    }
}
