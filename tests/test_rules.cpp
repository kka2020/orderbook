#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "book_test_support.h"
#include "lob/baseline.h"
#include "lob/fast_book.h"

namespace lob::test {
    template <typename Book>
    class RulesTest : public BookTest<Book> {};

    using BookTypes = ::testing::Types<BaselineBook<RecordingListener>, FastBook<RecordingListener>>;
    TYPED_TEST_SUITE(RulesTest, BookTypes);

    // ---- Resting orders and best bid/ask ----

    TYPED_TEST(RulesTest, BestBidIsNullOptWhenNoRestingBuys) {
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, BestBidIsHighestPriceAmongRestingBuys) {
        this->send(buy(OrderId{2}, Price{2}, Quantity{5}));
        this->send(buy(OrderId{1}, Price{3}, Quantity{5}));
        this->send(buy(OrderId{3}, Price{1}, Quantity{5}));

        EXPECT_EQ(this->book.best_bid(), Price{3});
    }

    TYPED_TEST(RulesTest, BestAskIsNullOptWhenNoRestingSells) {
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, BestAskIsLowestPriceAmongRestingSells) {
        this->send(sell(OrderId{2}, Price{2}, Quantity{5}));
        this->send(sell(OrderId{3}, Price{1}, Quantity{5}));
        this->send(sell(OrderId{1}, Price{3}, Quantity{5}));

        EXPECT_EQ(this->book.best_ask(), Price{1});
    }

    // ---- Acknowledgements ----

    TYPED_TEST(RulesTest, NewOrderIsAcknowledgedWithItsOwnFields) {
        SeqNo seq = this->send(buy(OrderId{10}, Price{50}, Quantity{7}));

        std::vector<Event> expected {
            OrderAck{.side = Side::Buy, .qty = Quantity{7}, .seq = seq, .id = OrderId{10}, .price = Price{50}},
        };
        EXPECT_EQ(this->listener.events, expected);
    }

    TYPED_TEST(RulesTest, NonCrossingOrdersRestWithoutTrading) {
        this->send(buy(OrderId{10}, Price{49}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{5}));

        EXPECT_TRUE(this->trades().empty());
        EXPECT_EQ(this->book.best_bid(), Price{49});
        EXPECT_EQ(this->book.best_ask(), Price{50});
    }

    // ---- Crossing: a trade happens when buy price >= sell price ----

    TYPED_TEST(RulesTest, BuyAtAskPriceTradesFully) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{10}));
        SeqNo taker = this->send(buy(OrderId{20}, Price{50}, Quantity{10}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{10}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, SellAtBidPriceTradesFully) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{10}));
        SeqNo taker = this->send(sell(OrderId{20}, Price{50}, Quantity{10}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{10}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, AggressiveBuyFillsAtMakerPrice) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{10}));
        this->send(buy(OrderId{20}, Price{52}, Quantity{10}));

        ASSERT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->trades()[0].price, Price{50});
    }

    TYPED_TEST(RulesTest, AggressiveSellFillsAtMakerPrice) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{10}));
        this->send(sell(OrderId{20}, Price{48}, Quantity{10}));

        ASSERT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->trades()[0].price, Price{50});
    }

    TYPED_TEST(RulesTest, OrderAckComesBeforeTheTradesItCauses) {
        SeqNo maker = this->send(sell(OrderId{10}, Price{50}, Quantity{10}));
        SeqNo taker = this->send(buy(OrderId{20}, Price{51}, Quantity{10}));

        std::vector<Event> expected {
            OrderAck{.side = Side::Sell, .qty = Quantity{10}, .seq = maker, .id = OrderId{10}, .price = Price{50}},
            OrderAck{.side = Side::Buy, .qty = Quantity{10}, .seq = taker, .id = OrderId{20}, .price = Price{51}},
            Trade{.qty = Quantity{10}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->listener.events, expected);
    }

    // ---- Partial fills ----

    TYPED_TEST(RulesTest, PartialFillLeavesMakerRemainderResting) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{100}));
        SeqNo taker = this->send(buy(OrderId{20}, Price{51}, Quantity{60}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{60}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_ask(), Price{50});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 40u);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, PartialFillRestsTakerRemainderAtItsOwnPrice) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{40}));
        SeqNo taker = this->send(buy(OrderId{20}, Price{51}, Quantity{100}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{40}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
        EXPECT_EQ(this->book.best_bid(), Price{51});
        EXPECT_EQ(this->book.depth_at(Price{51}, Side::Buy), 60u);
    }

    // ---- Priority: better price first, then earlier arrival ----

    TYPED_TEST(RulesTest, BetterPriceFillsFirstEvenIfItArrivedLater) {
        this->send(sell(OrderId{10}, Price{51}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{30}, Price{51}, Quantity{5}));

        ASSERT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->trades()[0].maker_id, OrderId{20});
        EXPECT_EQ(this->trades()[0].price, Price{50});
        EXPECT_EQ(this->book.best_ask(), Price{51});
    }

    TYPED_TEST(RulesTest, EarlierOrderAtSamePriceFillsFirst) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{30}, Price{50}, Quantity{5}));

        ASSERT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->trades()[0].maker_id, OrderId{10});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 5u);
    }

    TYPED_TEST(RulesTest, TakerWalksTheQueueAtOnePriceInArrivalOrder) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{3}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{4}));
        this->send(sell(OrderId{30}, Price{50}, Quantity{5}));
        SeqNo taker = this->send(buy(OrderId{40}, Price{50}, Quantity{10}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{3}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{40}, .price = Price{50}},
            Trade{.qty = Quantity{4}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{40}, .price = Price{50}},
            Trade{.qty = Quantity{3}, .seq = taker, .maker_id = OrderId{30}, .taker_id = OrderId{40}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 2u);
    }

    // ---- Cleanup: filled orders and emptied levels must disappear ----

    TYPED_TEST(RulesTest, FullyFilledMakerIsRemovedFromItsQueue) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{30}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{40}, Price{50}, Quantity{5}));

        ASSERT_EQ(this->trades().size(), 2u);
        EXPECT_EQ(this->trades()[1].maker_id, OrderId{20});
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, EmptiedLevelIsRemovedSoNextLevelBecomesBest) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{51}, Quantity{5}));
        this->send(buy(OrderId{30}, Price{50}, Quantity{5}));

        EXPECT_EQ(this->book.best_ask(), Price{51});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 0u);
    }

    // ---- Multi-level sweeps: one taker eats several levels, best first, up to its limit ----

    TYPED_TEST(RulesTest, BuySweepsAskLevelsFromLowestUp) {
        // Inserted out of price order, so the sweep order must come from price, not arrival
        this->send(sell(OrderId{10}, Price{52}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{30}, Price{51}, Quantity{5}));
        SeqNo taker = this->send(buy(OrderId{40}, Price{52}, Quantity{15}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{40}, .price = Price{50}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{30}, .taker_id = OrderId{40}, .price = Price{51}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{40}, .price = Price{52}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, SellSweepsBidLevelsFromHighestDown) {
        this->send(buy(OrderId{10}, Price{48}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{30}, Price{49}, Quantity{5}));
        SeqNo taker = this->send(sell(OrderId{40}, Price{48}, Quantity{15}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{40}, .price = Price{50}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{30}, .taker_id = OrderId{40}, .price = Price{49}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{40}, .price = Price{48}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, BuySweepStopsAtItsLimitAndRestsTheRemainder) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{51}, Quantity{5}));
        this->send(sell(OrderId{30}, Price{53}, Quantity{5}));
        SeqNo taker = this->send(buy(OrderId{40}, Price{52}, Quantity{20}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{40}, .price = Price{50}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{40}, .price = Price{51}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_ask(), Price{53});
        EXPECT_EQ(this->book.depth_at(Price{53}, Side::Sell), 5u);
        EXPECT_EQ(this->book.best_bid(), Price{52});
        EXPECT_EQ(this->book.depth_at(Price{52}, Side::Buy), 10u);
    }

    TYPED_TEST(RulesTest, SellSweepStopsAtItsLimitAndRestsTheRemainder) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{49}, Quantity{5}));
        this->send(buy(OrderId{30}, Price{47}, Quantity{5}));
        SeqNo taker = this->send(sell(OrderId{40}, Price{48}, Quantity{20}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{40}, .price = Price{50}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{40}, .price = Price{49}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_bid(), Price{47});
        EXPECT_EQ(this->book.depth_at(Price{47}, Side::Buy), 5u);
        EXPECT_EQ(this->book.best_ask(), Price{48});
        EXPECT_EQ(this->book.depth_at(Price{48}, Side::Sell), 10u);
    }

    TYPED_TEST(RulesTest, SweepEndingMidLevelLeavesThatMakerPartlyFilled) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{51}, Quantity{10}));
        SeqNo taker = this->send(buy(OrderId{30}, Price{51}, Quantity{8}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{30}, .price = Price{50}},
            Trade{.qty = Quantity{3}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{30}, .price = Price{51}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_ask(), Price{51});
        EXPECT_EQ(this->book.depth_at(Price{51}, Side::Sell), 7u);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, SweepKeepsTimePriorityWithinEachLevel) {
        this->send(sell(OrderId{10}, Price{51}, Quantity{2}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{2}));
        this->send(sell(OrderId{30}, Price{51}, Quantity{2}));
        this->send(sell(OrderId{40}, Price{50}, Quantity{2}));
        SeqNo taker = this->send(buy(OrderId{50}, Price{51}, Quantity{7}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{2}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{50}, .price = Price{50}},
            Trade{.qty = Quantity{2}, .seq = taker, .maker_id = OrderId{40}, .taker_id = OrderId{50}, .price = Price{50}},
            Trade{.qty = Quantity{2}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{50}, .price = Price{51}},
            Trade{.qty = Quantity{1}, .seq = taker, .maker_id = OrderId{30}, .taker_id = OrderId{50}, .price = Price{51}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.depth_at(Price{51}, Side::Sell), 1u);
    }

    TYPED_TEST(RulesTest, SweepThatClearsTheWholeSideRestsTheRemainder) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{51}, Quantity{5}));
        this->send(buy(OrderId{30}, Price{55}, Quantity{12}));

        EXPECT_EQ(this->trades().size(), 2u);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
        EXPECT_EQ(this->book.best_bid(), Price{55});
        EXPECT_EQ(this->book.depth_at(Price{55}, Side::Buy), 2u);
    }

    // ---- IOC: trade what crosses now, cancel the rest (never rests) ----

    TYPED_TEST(RulesTest, IocFullyFilledEmitsNoCancelAck) {
        SeqNo maker = this->send(sell(OrderId{10}, Price{50}, Quantity{10}));
        SeqNo taker = this->send(ioc_buy(OrderId{20}, Price{50}, Quantity{10}));

        std::vector<Event> expected {
            OrderAck{.side = Side::Sell, .qty = Quantity{10}, .seq = maker, .id = OrderId{10}, .price = Price{50}},
            OrderAck{.side = Side::Buy, .qty = Quantity{10}, .seq = taker, .id = OrderId{20}, .price = Price{50}},
            Trade{.qty = Quantity{10}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->listener.events, expected);
    }

    TYPED_TEST(RulesTest, IocPartialFillCancelsTheRemainderAfterItsTrades) {
        SeqNo maker = this->send(sell(OrderId{10}, Price{50}, Quantity{4}));
        SeqNo taker = this->send(ioc_buy(OrderId{20}, Price{51}, Quantity{10}));

        std::vector<Event> expected {
            OrderAck{.side = Side::Sell, .qty = Quantity{4}, .seq = maker, .id = OrderId{10}, .price = Price{50}},
            OrderAck{.side = Side::Buy, .qty = Quantity{10}, .seq = taker, .id = OrderId{20}, .price = Price{51}},
            Trade{.qty = Quantity{4}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
            CancelAck{.side = Side::Buy, .qty = Quantity{6}, .seq = taker, .id = OrderId{20}, .price = Price{51}},
        };
        EXPECT_EQ(this->listener.events, expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, IocSellPartialFillCancelsTheRemainder) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo taker = this->send(ioc_sell(OrderId{20}, Price{50}, Quantity{8}));

        std::vector<CancelAck> expected {
            CancelAck{.side = Side::Sell, .qty = Quantity{3}, .seq = taker, .id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->cancel_acks(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, IocThatDoesNotCrossIsCancelledInFull) {
        this->send(sell(OrderId{10}, Price{52}, Quantity{5}));
        SeqNo taker = this->send(ioc_buy(OrderId{20}, Price{50}, Quantity{10}));

        std::vector<CancelAck> expected {
            CancelAck{.side = Side::Buy, .qty = Quantity{10}, .seq = taker, .id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_TRUE(this->trades().empty());
        EXPECT_EQ(this->cancel_acks(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), Price{52});
        EXPECT_EQ(this->book.depth_at(Price{52}, Side::Sell), 5u);
    }

    TYPED_TEST(RulesTest, IocSweepStopsAtItsLimitAndCancelsTheRest) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{52}, Quantity{5}));
        SeqNo taker = this->send(ioc_buy(OrderId{30}, Price{51}, Quantity{10}));

        std::vector<CancelAck> expected {
            CancelAck{.side = Side::Buy, .qty = Quantity{5}, .seq = taker, .id = OrderId{30}, .price = Price{51}},
        };
        ASSERT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->trades()[0].maker_id, OrderId{10});
        EXPECT_EQ(this->cancel_acks(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), Price{52});
    }

    // ---- Market: like IOC, but with no limit price (the price field is ignored) ----

    TYPED_TEST(RulesTest, MarketBuySweepsEveryLevelWhateverThePrice) {
        // The market order is sent with price 0; as a limit buy it would cross nothing
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{60}, Quantity{5}));
        this->send(sell(OrderId{30}, Price{1000}, Quantity{5}));
        SeqNo taker = this->send(market_buy(OrderId{40}, Quantity{15}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{40}, .price = Price{50}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{40}, .price = Price{60}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{30}, .taker_id = OrderId{40}, .price = Price{1000}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_TRUE(this->cancel_acks().empty());
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, MarketSellIgnoresItsPriceField) {
        // As a limit sell at 100 this would not cross the bid at 50
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo taker = this->send(new_order(Side::Sell, OrderId{20}, Price{100}, Quantity{5}, TimeInForce::Market));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, MarketRemainderIsCancelledNotRested) {
        SeqNo maker = this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo taker = this->send(market_buy(OrderId{20}, Quantity{8}));

        std::vector<Event> expected {
            OrderAck{.side = Side::Sell, .qty = Quantity{5}, .seq = maker, .id = OrderId{10}, .price = Price{50}},
            OrderAck{.side = Side::Buy, .qty = Quantity{8}, .seq = taker, .id = OrderId{20}, .price = Price{0}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
            CancelAck{.side = Side::Buy, .qty = Quantity{3}, .seq = taker, .id = OrderId{20}, .price = Price{0}},
        };
        EXPECT_EQ(this->listener.events, expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, MarketIntoEmptyBookIsCancelledInFull) {
        SeqNo taker = this->send(market_sell(OrderId{10}, Quantity{7}));

        std::vector<Event> expected {
            OrderAck{.side = Side::Sell, .qty = Quantity{7}, .seq = taker, .id = OrderId{10}, .price = Price{0}},
            CancelAck{.side = Side::Sell, .qty = Quantity{7}, .seq = taker, .id = OrderId{10}, .price = Price{0}},
        };
        EXPECT_EQ(this->listener.events, expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    // ---- Cancel: remove a resting order by id ----

    TYPED_TEST(RulesTest, CancelOfRestingOrderEmitsCancelAckAndEmptiesTheBook) {
        SeqNo placed = this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo cancelled = this->send(cancel(OrderId{10}));

        std::vector<Event> expected {
            OrderAck{.side = Side::Buy, .qty = Quantity{5}, .seq = placed, .id = OrderId{10}, .price = Price{50}},
            CancelAck{.side = Side::Buy, .qty = Quantity{5}, .seq = cancelled, .id = OrderId{10}, .price = Price{50}},
        };
        EXPECT_EQ(this->listener.events, expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Buy), 0u);
    }

    TYPED_TEST(RulesTest, CancelOfPartlyFilledMakerReportsWhatWasStillResting) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{10}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{4}));
        SeqNo cancelled = this->send(cancel(OrderId{10}));

        std::vector<CancelAck> expected {
            CancelAck{.side = Side::Sell, .qty = Quantity{6}, .seq = cancelled, .id = OrderId{10}, .price = Price{50}},
        };
        EXPECT_EQ(this->cancel_acks(), expected);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, CancelOfTakersRestingRemainderReportsTheRemainder) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{4}));
        this->send(buy(OrderId{20}, Price{51}, Quantity{10}));
        SeqNo cancelled = this->send(cancel(OrderId{20}));

        std::vector<CancelAck> expected {
            CancelAck{.side = Side::Buy, .qty = Quantity{6}, .seq = cancelled, .id = OrderId{20}, .price = Price{51}},
        };
        EXPECT_EQ(this->cancel_acks(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, CancellingTheOnlyOrderAtBestLeavesNoPhantomLevel) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{52}, Quantity{5}));
        this->send(cancel(OrderId{10}));

        EXPECT_EQ(this->book.best_ask(), Price{52});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 0u);

        // A buy at 50 must not trade against the cancelled order
        this->send(buy(OrderId{30}, Price{50}, Quantity{5}));
        EXPECT_TRUE(this->trades().empty());
        EXPECT_EQ(this->book.best_bid(), Price{50});
    }

    TYPED_TEST(RulesTest, CancelInTheMiddleOfALevelKeepsTheOthersInArrivalOrder) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{30}, Price{50}, Quantity{5}));
        this->send(cancel(OrderId{20}));

        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 10u);

        SeqNo taker = this->send(buy(OrderId{40}, Price{50}, Quantity{10}));
        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{10}, .taker_id = OrderId{40}, .price = Price{50}},
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{30}, .taker_id = OrderId{40}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, CancelOfFrontOrderLetsTheNextOneFillFirst) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        this->send(cancel(OrderId{10}));

        SeqNo taker = this->send(sell(OrderId{30}, Price{50}, Quantity{5}));
        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{30}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    // ---- Cancel rejects: the id is not resting right now; the book must not change ----

    TYPED_TEST(RulesTest, CancelOfUnknownIdIsRejectedAndChangesNothing) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo cancelled = this->send(cancel(OrderId{99}));

        std::vector<CancelReject> expected {CancelReject{.seq = cancelled, .id = OrderId{99}}};
        EXPECT_EQ(this->cancel_rejects(), expected);
        EXPECT_TRUE(this->cancel_acks().empty());
        EXPECT_EQ(this->book.best_bid(), Price{50});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Buy), 5u);
    }

    TYPED_TEST(RulesTest, CancelOfFullyFilledMakerIsRejected) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        SeqNo cancelled = this->send(cancel(OrderId{10}));

        std::vector<CancelReject> expected {CancelReject{.seq = cancelled, .id = OrderId{10}}};
        EXPECT_EQ(this->cancel_rejects(), expected);
        EXPECT_TRUE(this->cancel_acks().empty());
    }

    TYPED_TEST(RulesTest, CancelOfFullyFilledTakerIsRejected) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        SeqNo cancelled = this->send(cancel(OrderId{20}));

        std::vector<CancelReject> expected {CancelReject{.seq = cancelled, .id = OrderId{20}}};
        EXPECT_EQ(this->cancel_rejects(), expected);
        EXPECT_TRUE(this->cancel_acks().empty());
    }

    TYPED_TEST(RulesTest, SecondCancelOfTheSameOrderIsRejected) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(cancel(OrderId{10}));
        SeqNo second = this->send(cancel(OrderId{10}));

        std::vector<CancelReject> expected {CancelReject{.seq = second, .id = OrderId{10}}};
        EXPECT_EQ(this->cancel_rejects(), expected);
        EXPECT_EQ(this->cancel_acks().size(), 1u);
    }

    TYPED_TEST(RulesTest, CancelOfIocRemainderIsRejectedBecauseItNeverRested) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{4}));
        this->send(ioc_buy(OrderId{20}, Price{50}, Quantity{10}));
        SeqNo cancelled = this->send(cancel(OrderId{20}));

        std::vector<CancelReject> expected {CancelReject{.seq = cancelled, .id = OrderId{20}}};
        EXPECT_EQ(this->cancel_rejects(), expected);
        EXPECT_EQ(this->cancel_acks().size(), 1u);  // only the IOC remainder's own CancelAck
    }

    // ---- Id reuse: once an order is gone, its id is free again ----

    TYPED_TEST(RulesTest, IdCanBeReusedAfterCancel) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(cancel(OrderId{10}));
        this->send(buy(OrderId{10}, Price{48}, Quantity{7}));

        EXPECT_EQ(this->book.best_bid(), Price{48});

        SeqNo cancelled = this->send(cancel(OrderId{10}));
        CancelAck expected {.side = Side::Buy, .qty = Quantity{7}, .seq = cancelled, .id = OrderId{10}, .price = Price{48}};
        ASSERT_EQ(this->cancel_acks().size(), 2u);
        EXPECT_EQ(this->cancel_acks()[1], expected);
        EXPECT_TRUE(this->cancel_rejects().empty());
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, IdCanBeReusedAfterFullFill) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{10}, Price{55}, Quantity{3}));

        EXPECT_EQ(this->book.best_ask(), Price{55});

        SeqNo cancelled = this->send(cancel(OrderId{10}));
        std::vector<CancelAck> expected {
            CancelAck{.side = Side::Sell, .qty = Quantity{3}, .seq = cancelled, .id = OrderId{10}, .price = Price{55}},
        };
        EXPECT_EQ(this->cancel_acks(), expected);
        EXPECT_TRUE(this->cancel_rejects().empty());
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    // ---- Storage reuse: a removed order's memory may be handed to the next order ----
    //
    // A pooled book (v2 step 2 on) reuses the slot of a cancelled or filled order. Nothing of the
    // old order may survive in it: not its place in a queue, not its id, price or side. And time
    // priority must come from arrival order, never from which slot an order happened to get.

    TYPED_TEST(RulesTest, ACancelledOrdersStorageKeepsNoTraceOfIt) {
        this->send(buy(OrderId{10}, Price{100}, Quantity{5}));
        this->send(cancel(OrderId{10}));
        this->send(sell(OrderId{20}, Price{105}, Quantity{3}));  // may reuse id 10's storage

        // No ghost of id 10 at 100: a sell at 100 finds no bid, so it rests instead of trading.
        this->send(sell(OrderId{30}, Price{100}, Quantity{5}));
        EXPECT_TRUE(this->trades().empty());
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.depth_at(Price{100}, Side::Buy), 0u);

        // id 10 is gone; id 20 is intact with its own fields.
        this->send(cancel(OrderId{10}));
        const SeqNo cancelled = this->send(cancel(OrderId{20}));
        const std::vector<CancelAck> expected {
            CancelAck{.side = Side::Buy, .qty = Quantity{5}, .seq = SeqNo{2}, .id = OrderId{10}, .price = Price{100}},
            CancelAck{.side = Side::Sell, .qty = Quantity{3}, .seq = cancelled, .id = OrderId{20}, .price = Price{105}},
        };
        EXPECT_EQ(this->cancel_acks(), expected);
        EXPECT_EQ(this->cancel_rejects().size(), 1u);
    }

    TYPED_TEST(RulesTest, AFilledOrdersStorageKeepsNoTraceOfIt) {
        this->send(buy(OrderId{10}, Price{100}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{100}, Quantity{5}));  // fills id 10 completely
        this->send(buy(OrderId{30}, Price{99}, Quantity{2}));    // may reuse id 10's storage

        const SeqNo hit = this->send(sell(OrderId{40}, Price{99}, Quantity{2}));
        const std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = SeqNo{2}, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{100}},
            Trade{.qty = Quantity{2}, .seq = hit, .maker_id = OrderId{30}, .taker_id = OrderId{40}, .price = Price{99}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    // Free storage is often handed out last-freed-first, so the three new orders below may get the
    // old slots in reverse. They must still fill in the order they arrived.
    TYPED_TEST(RulesTest, ReusedStorageDoesNotChangeTimePriority) {
        this->send(buy(OrderId{10}, Price{100}, Quantity{1}));
        this->send(buy(OrderId{20}, Price{100}, Quantity{1}));
        this->send(buy(OrderId{30}, Price{100}, Quantity{1}));
        this->send(cancel(OrderId{10}));
        this->send(cancel(OrderId{20}));
        this->send(cancel(OrderId{30}));

        this->send(buy(OrderId{40}, Price{100}, Quantity{1}));
        this->send(buy(OrderId{50}, Price{100}, Quantity{1}));
        this->send(buy(OrderId{60}, Price{100}, Quantity{1}));
        const SeqNo hit = this->send(sell(OrderId{70}, Price{100}, Quantity{3}));

        const std::vector<Trade> expected {
            Trade{.qty = Quantity{1}, .seq = hit, .maker_id = OrderId{40}, .taker_id = OrderId{70}, .price = Price{100}},
            Trade{.qty = Quantity{1}, .seq = hit, .maker_id = OrderId{50}, .taker_id = OrderId{70}, .price = Price{100}},
            Trade{.qty = Quantity{1}, .seq = hit, .maker_id = OrderId{60}, .taker_id = OrderId{70}, .price = Price{100}},
        };
        EXPECT_EQ(this->trades(), expected);
    }

    // Same, with storage freed by fills spread over two levels and refilled on the other side.
    TYPED_TEST(RulesTest, StorageFreedByFillsServesTheOtherSideInArrivalOrder) {
        this->send(buy(OrderId{10}, Price{100}, Quantity{1}));
        this->send(buy(OrderId{20}, Price{99}, Quantity{1}));
        this->send(sell(OrderId{30}, Price{99}, Quantity{2}));  // fills 10 then 20

        this->send(sell(OrderId{40}, Price{105}, Quantity{1}));
        this->send(sell(OrderId{50}, Price{105}, Quantity{1}));
        const SeqNo hit = this->send(buy(OrderId{60}, Price{105}, Quantity{2}));

        const std::vector<Trade> fills = this->trades();
        ASSERT_EQ(fills.size(), 4u);
        EXPECT_EQ(fills[2], (Trade{.qty = Quantity{1}, .seq = hit, .maker_id = OrderId{40}, .taker_id = OrderId{60}, .price = Price{105}}));
        EXPECT_EQ(fills[3], (Trade{.qty = Quantity{1}, .seq = hit, .maker_id = OrderId{50}, .taker_id = OrderId{60}, .price = Price{105}}));
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    // ---- Queue links: removing an order from anywhere in a level ----
    //
    // A linked queue (std::list, or v2 step 3's links stored inside the orders) removes an order by
    // re-pointing its neighbours at each other. Front, back, middle and only-order removals are
    // different cases, and so are removal by cancel and removal by a fill. Each test below
    // finishes by filling the level away, so the trade order shows exactly what the queue holds.

    // Maker ids of the trades, in emission order.
    inline std::vector<OrderId> makers_of(const std::vector<Trade>& trades) {
        std::vector<OrderId> out;
        for (const Trade& t : trades) {
            out.push_back(t.maker_id);
        }
        return out;
    }

    // Every ordered choice of 0..5 orders to cancel from a five-order level (326 cases), then a
    // new order joins, then a market order fills the level away. The queue must be exactly the
    // survivors in arrival order followed by the newcomer, and depth must match it throughout.
    TYPED_TEST(RulesTest, EveryWayOfCancellingFromALevelKeepsTheRestInArrivalOrder) {
        constexpr std::size_t n = 5;
        std::array<std::size_t, n> order {0, 1, 2, 3, 4};
        auto id_of = [](std::size_t i) { return OrderId{10 * (i + 1)}; };  // 10, 20, ..., 50
        auto qty_of = [](std::size_t i) { return Quantity{static_cast<std::uint32_t>(i + 1)}; };       // distinct, so depth is checked too

        do {
            for (std::size_t k = 0; k <= n; ++k) {
                // Cancel order[0..k). Skip permutations that only reorder the uncancelled tail,
                // so each ordered choice runs once.
                if (!std::is_sorted(order.begin() + static_cast<std::ptrdiff_t>(k), order.end())) {
                    continue;
                }

                RecordingListener listener;
                TypeParam book {listener};
                std::uint64_t seq = 1;
                auto send = [&](auto command) {
                    command.seq = SeqNo{seq++};
                    book.apply(command);
                };

                std::string trace = "cancelled (by index, in order):";
                for (std::size_t j = 0; j < k; ++j) {
                    trace += " " + std::to_string(order[j]);
                }
                SCOPED_TRACE(trace);

                for (std::size_t i = 0; i < n; ++i) {
                    send(sell(id_of(i), Price{100}, qty_of(i)));
                }
                std::array<bool, n> cancelled {};
                for (std::size_t j = 0; j < k; ++j) {
                    send(cancel(id_of(order[j])));
                    cancelled[order[j]] = true;
                }

                std::vector<OrderId> expected;
                std::uint64_t depth = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    if (!cancelled[i]) {
                        expected.push_back(id_of(i));
                        depth += qty_of(i).value;
                    }
                }
                ASSERT_EQ(book.depth_at(Price{100}, Side::Sell), depth);
                ASSERT_EQ(book.best_ask(), k == n ? std::nullopt : std::optional{Price{100}});

                // A newcomer must join at the back, whatever was removed before it.
                send(sell(OrderId{60}, Price{100}, Quantity{10}));
                expected.push_back(OrderId{60});
                ASSERT_EQ(book.depth_at(Price{100}, Side::Sell), depth + 10);

                listener.events.clear();
                send(market_buy(OrderId{70}, Quantity{1000}));
                std::vector<Trade> trades;
                for (const Event& e : listener.events) {
                    if (const auto* t = std::get_if<Trade>(&e)) {
                        trades.push_back(*t);
                    }
                }
                EXPECT_EQ(makers_of(trades), expected);
                EXPECT_EQ(book.best_ask(), std::nullopt);
                EXPECT_EQ(book.depth_at(Price{100}, Side::Sell), 0u);
            }
        } while (std::next_permutation(order.begin(), order.end()));
    }

    // A fill removes the front order a different way from a cancel. The order behind it must then
    // be the new front for a cancel, and a newcomer must still join at the back.
    TYPED_TEST(RulesTest, AfterAFillRemovesTheFrontTheNextOrderIsTheFront) {
        this->send(sell(OrderId{10}, Price{100}, Quantity{2}));
        this->send(sell(OrderId{20}, Price{100}, Quantity{3}));
        this->send(sell(OrderId{30}, Price{100}, Quantity{4}));
        this->send(buy(OrderId{40}, Price{100}, Quantity{2}));  // fills 10 exactly
        this->send(cancel(OrderId{20}));                         // now the front
        this->send(sell(OrderId{50}, Price{100}, Quantity{5}));

        EXPECT_EQ(this->book.depth_at(Price{100}, Side::Sell), 9u);
        this->send(market_buy(OrderId{60}, Quantity{100}));
        const std::vector<OrderId> expected {OrderId{10}, OrderId{30}, OrderId{50}};
        EXPECT_EQ(makers_of(this->trades()), expected);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    // A partly filled front stays the front; cancelling the order right behind it must not lose it.
    TYPED_TEST(RulesTest, CancellingTheOrderBehindAPartlyFilledFrontKeepsTheFront) {
        this->send(buy(OrderId{10}, Price{100}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{100}, Quantity{5}));
        this->send(buy(OrderId{30}, Price{100}, Quantity{5}));
        this->send(sell(OrderId{40}, Price{100}, Quantity{2}));  // 10 is left with 3
        this->send(cancel(OrderId{20}));

        EXPECT_EQ(this->book.depth_at(Price{100}, Side::Buy), 8u);
        const SeqNo hit = this->send(market_sell(OrderId{50}, Quantity{100}));
        const std::vector<Trade> all = this->trades();
        ASSERT_EQ(all.size(), 3u);
        const std::vector<Trade> sweep {all.begin() + 1, all.end()};  // skip 40's partial fill
        const std::vector<Trade> expected {
            Trade{.qty = Quantity{3}, .seq = hit, .maker_id = OrderId{10}, .taker_id = OrderId{50}, .price = Price{100}},
            Trade{.qty = Quantity{5}, .seq = hit, .maker_id = OrderId{30}, .taker_id = OrderId{50}, .price = Price{100}},
        };
        EXPECT_EQ(sweep, expected);
    }

    // The storage of an order cancelled from the middle of one level is handed to an order at
    // another price. That order must not carry the old neighbours with it: the old level keeps only
    // its own orders, and the new order is alone at its own level.
    TYPED_TEST(RulesTest, StorageFreedMidQueueAndReusedAtAnotherPriceKeepsNoOldNeighbours) {
        this->send(sell(OrderId{10}, Price{100}, Quantity{1}));
        this->send(sell(OrderId{20}, Price{100}, Quantity{1}));
        this->send(sell(OrderId{30}, Price{100}, Quantity{1}));
        this->send(cancel(OrderId{20}));
        this->send(sell(OrderId{40}, Price{101}, Quantity{7}));  // may reuse 20's storage
        this->send(sell(OrderId{50}, Price{100}, Quantity{1}));

        EXPECT_EQ(this->book.depth_at(Price{100}, Side::Sell), 3u);
        EXPECT_EQ(this->book.depth_at(Price{101}, Side::Sell), 7u);

        // Fill only the 100 level: 40 must not be reached through a stale link.
        this->send(buy(OrderId{60}, Price{100}, Quantity{100}));
        const std::vector<OrderId> at_100 {OrderId{10}, OrderId{30}, OrderId{50}};
        EXPECT_EQ(makers_of(this->trades()), at_100);
        EXPECT_EQ(this->book.best_ask(), Price{101});
        EXPECT_EQ(this->book.best_bid(), Price{100});  // 60's remainder rests

        // 40 is intact and alone at 101.
        this->send(market_buy(OrderId{70}, Quantity{100}));
        ASSERT_EQ(this->trades().size(), 4u);
        EXPECT_EQ(this->trades().back().maker_id, OrderId{40});
        EXPECT_EQ(this->trades().back().qty, Quantity{7});
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    // Modify removes the order and adds it again at the back. Doing that from the middle, the front
    // and the back (where the order is removed and re-added as the back of the same level) must
    // each leave a well-formed queue.
    TYPED_TEST(RulesTest, ModifyFromMiddleFrontAndBackEachRequeuesAtTheBack) {
        this->send(buy(OrderId{10}, Price{100}, Quantity{1}));
        this->send(buy(OrderId{20}, Price{100}, Quantity{1}));
        this->send(buy(OrderId{30}, Price{100}, Quantity{1}));
        this->send(buy(OrderId{40}, Price{100}, Quantity{1}));
        this->send(modify(OrderId{20}, Side::Buy, Price{100}, Quantity{2}));  // middle: 10 30 40 20
        this->send(modify(OrderId{10}, Side::Buy, Price{100}, Quantity{3}));  // front:  30 40 20 10
        this->send(modify(OrderId{10}, Side::Buy, Price{100}, Quantity{4}));  // back:   30 40 20 10

        EXPECT_EQ(this->book.depth_at(Price{100}, Side::Buy), 8u);
        this->send(market_sell(OrderId{50}, Quantity{100}));
        const std::vector<OrderId> expected {OrderId{30}, OrderId{40}, OrderId{20}, OrderId{10}};
        EXPECT_EQ(makers_of(this->trades()), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    // ---- Modify: cancel + reinsert at the new price/quantity (loses time priority) ----

    TYPED_TEST(RulesTest, ModifyRepricesARestingOrderAndAcks) {
        SeqNo placed = this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo modified = this->send(modify(OrderId{10}, Side::Buy, Price{52}, Quantity{5}));

        // No OrderAck or CancelAck for the cancel + reinsert: just the ModifyAck
        std::vector<Event> expected {
            OrderAck{.side = Side::Buy, .qty = Quantity{5}, .seq = placed, .id = OrderId{10}, .price = Price{50}},
            ModifyAck{.side = Side::Buy, .qty = Quantity{5}, .seq = modified, .id = OrderId{10}, .price = Price{52}},
        };
        EXPECT_EQ(this->listener.events, expected);
        EXPECT_EQ(this->book.best_bid(), Price{52});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Buy), 0u);
        EXPECT_EQ(this->book.depth_at(Price{52}, Side::Buy), 5u);
    }

    TYPED_TEST(RulesTest, ModifyChangesQuantityAtTheSamePrice) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo modified = this->send(modify(OrderId{10}, Side::Sell, Price{50}, Quantity{8}));

        std::vector<ModifyAck> expected {
            ModifyAck{.side = Side::Sell, .qty = Quantity{8}, .seq = modified, .id = OrderId{10}, .price = Price{50}},
        };
        EXPECT_EQ(this->modify_acks(), expected);
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 8u);
    }

    TYPED_TEST(RulesTest, ModifyLosesTimePriorityEvenAtTheSamePrice) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{50}, Quantity{5}));
        this->send(modify(OrderId{10}, Side::Sell, Price{50}, Quantity{6}));

        SeqNo taker = this->send(buy(OrderId{30}, Price{50}, Quantity{5}));
        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{30}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 6u);
    }

    TYPED_TEST(RulesTest, ModifiedOrderCanBeCancelledAtItsNewPlace) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(modify(OrderId{10}, Side::Buy, Price{48}, Quantity{7}));
        SeqNo cancelled = this->send(cancel(OrderId{10}));

        std::vector<CancelAck> expected {
            CancelAck{.side = Side::Buy, .qty = Quantity{7}, .seq = cancelled, .id = OrderId{10}, .price = Price{48}},
        };
        EXPECT_EQ(this->cancel_acks(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, ModifyOfPartlyFilledOrderRestsTheNewQuantity) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{10}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{4}));  // 6 left resting
        SeqNo modified = this->send(modify(OrderId{10}, Side::Sell, Price{51}, Quantity{7}));

        std::vector<ModifyAck> expected {
            ModifyAck{.side = Side::Sell, .qty = Quantity{7}, .seq = modified, .id = OrderId{10}, .price = Price{51}},
        };
        EXPECT_EQ(this->modify_acks(), expected);
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 0u);
        EXPECT_EQ(this->book.depth_at(Price{51}, Side::Sell), 7u);
    }

    TYPED_TEST(RulesTest, RepricingOneOfTwoSamePriceOrdersKeepsTheSibling) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        this->send(modify(OrderId{10}, Side::Buy, Price{49}, Quantity{5}));

        EXPECT_EQ(this->book.best_bid(), Price{50});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Buy), 5u);
        EXPECT_EQ(this->book.depth_at(Price{49}, Side::Buy), 5u);

        SeqNo taker = this->send(sell(OrderId{30}, Price{50}, Quantity{5}));
        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = taker, .maker_id = OrderId{20}, .taker_id = OrderId{30}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
    }

    // ---- Modify that crosses: trades at the maker's price first, then the ModifyAck ----

    TYPED_TEST(RulesTest, BuyModifyThatCrossesTradesAtMakerPriceThenAcks) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{45}, Quantity{5}));
        auto before = this->listener.events.size();
        SeqNo modified = this->send(modify(OrderId{20}, Side::Buy, Price{52}, Quantity{5}));

        std::vector<Event> expected {
            Trade{.qty = Quantity{5}, .seq = modified, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
            ModifyAck{.side = Side::Buy, .qty = Quantity{5}, .seq = modified, .id = OrderId{20}, .price = Price{52}},
        };
        std::vector<Event> emitted(this->listener.events.begin() + static_cast<std::ptrdiff_t>(before),
                                   this->listener.events.end());
        EXPECT_EQ(emitted, expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, SellModifyThatCrossesTradesAtMakerPrice) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{55}, Quantity{5}));
        SeqNo modified = this->send(modify(OrderId{20}, Side::Sell, Price{48}, Quantity{5}));

        std::vector<Trade> expected {
            Trade{.qty = Quantity{5}, .seq = modified, .maker_id = OrderId{10}, .taker_id = OrderId{20}, .price = Price{50}},
        };
        EXPECT_EQ(this->trades(), expected);
        EXPECT_EQ(this->modify_acks().size(), 1u);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, CrossingModifyRestsItsRemainderAtTheNewPrice) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{3}));
        this->send(buy(OrderId{20}, Price{45}, Quantity{8}));
        this->send(modify(OrderId{20}, Side::Buy, Price{51}, Quantity{8}));

        ASSERT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->trades()[0].qty, Quantity{3});
        EXPECT_EQ(this->book.best_bid(), Price{51});
        EXPECT_EQ(this->book.depth_at(Price{51}, Side::Buy), 5u);
        EXPECT_EQ(this->book.depth_at(Price{45}, Side::Buy), 0u);

        // The rested remainder is still reachable by id
        SeqNo cancelled = this->send(cancel(OrderId{20}));
        std::vector<CancelAck> expected {
            CancelAck{.side = Side::Buy, .qty = Quantity{5}, .seq = cancelled, .id = OrderId{20}, .price = Price{51}},
        };
        EXPECT_EQ(this->cancel_acks(), expected);
    }

    TYPED_TEST(RulesTest, FullyFilledModifyLeavesNothingToCancel) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{45}, Quantity{5}));
        this->send(modify(OrderId{20}, Side::Buy, Price{50}, Quantity{5}));
        SeqNo cancelled = this->send(cancel(OrderId{20}));

        std::vector<CancelReject> expected {CancelReject{.seq = cancelled, .id = OrderId{20}}};
        EXPECT_EQ(this->cancel_rejects(), expected);
    }

    // ---- Modify rejects: the book must not change ----

    TYPED_TEST(RulesTest, ModifyOfUnknownIdIsRejectedAndChangesNothing) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo modified = this->send(modify(OrderId{99}, Side::Buy, Price{52}, Quantity{5}));

        std::vector<ModifyReject> expected {ModifyReject{.seq = modified, .id = OrderId{99}}};
        EXPECT_EQ(this->modify_rejects(), expected);
        EXPECT_TRUE(this->modify_acks().empty());
        EXPECT_EQ(this->book.best_bid(), Price{50});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Buy), 5u);
    }

    TYPED_TEST(RulesTest, ModifyOfFullyFilledOrderIsRejected) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        SeqNo modified = this->send(modify(OrderId{10}, Side::Sell, Price{51}, Quantity{5}));

        std::vector<ModifyReject> expected {ModifyReject{.seq = modified, .id = OrderId{10}}};
        EXPECT_EQ(this->modify_rejects(), expected);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, ModifyOfCancelledOrderIsRejected) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(cancel(OrderId{10}));
        SeqNo modified = this->send(modify(OrderId{10}, Side::Buy, Price{50}, Quantity{6}));

        std::vector<ModifyReject> expected {ModifyReject{.seq = modified, .id = OrderId{10}}};
        EXPECT_EQ(this->modify_rejects(), expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, ModifyWithTheWrongSideIsRejectedAndLeavesTheOrderAlone) {
        this->send(sell(OrderId{10}, Price{55}, Quantity{3}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        // Order 20 is a buy. Ignoring the Modify's side would reprice it to a buy at 55 (which
        // crosses order 10); trusting the side would flip it to a sell. Both are wrong: reject.
        SeqNo modified = this->send(modify(OrderId{20}, Side::Sell, Price{55}, Quantity{5}));

        std::vector<ModifyReject> expected {ModifyReject{.seq = modified, .id = OrderId{20}}};
        EXPECT_EQ(this->modify_rejects(), expected);
        EXPECT_TRUE(this->modify_acks().empty());
        EXPECT_TRUE(this->trades().empty());
        EXPECT_EQ(this->book.best_bid(), Price{50});
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Buy), 5u);
        EXPECT_EQ(this->book.depth_at(Price{55}, Side::Sell), 3u);
    }

    // ---- Rejecting invalid new orders and modifies ----
    // A rejected new order gets exactly one event, OrderReject (no OrderAck, no Trade, no CancelAck),
    // and changes nothing. Checks run in this order and the first failure is the reason reported:
    // ZeroQuantity, then InvalidPrice (INT64_MIN/INT64_MAX; not checked for market orders), then
    // DuplicateId (the id is resting right now).

    constexpr Price min_price {std::numeric_limits<std::int64_t>::min()};
    constexpr Price max_price {std::numeric_limits<std::int64_t>::max()};

    TYPED_TEST(RulesTest, ZeroQuantityOrderIsRejectedInsteadOfAcked) {
        SeqNo sent = this->send(buy(OrderId{10}, Price{50}, Quantity{0}));

        std::vector<Event> expected {
            OrderReject{.reason = RejectReason::ZeroQuantity, .seq = sent, .id = OrderId{10}},
        };
        EXPECT_EQ(this->listener.events, expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, ZeroQuantityIocAndMarketOrdersAreRejectedWithoutACancelAck) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo ioc = this->send(ioc_buy(OrderId{20}, Price{50}, Quantity{0}));
        SeqNo market = this->send(market_buy(OrderId{30}, Quantity{0}));

        std::vector<OrderReject> expected {
            OrderReject{.reason = RejectReason::ZeroQuantity, .seq = ioc, .id = OrderId{20}},
            OrderReject{.reason = RejectReason::ZeroQuantity, .seq = market, .id = OrderId{30}},
        };
        EXPECT_EQ(this->order_rejects(), expected);
        EXPECT_TRUE(this->cancel_acks().empty());
        EXPECT_TRUE(this->trades().empty());
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 5u);
    }

    TYPED_TEST(RulesTest, PriceOfInt64MinOrMaxIsRejected) {
        SeqNo low = this->send(buy(OrderId{10}, min_price, Quantity{5}));
        SeqNo high = this->send(sell(OrderId{20}, max_price, Quantity{5}));
        SeqNo ioc = this->send(ioc_sell(OrderId{30}, min_price, Quantity{5}));

        std::vector<Event> expected {
            OrderReject{.reason = RejectReason::InvalidPrice, .seq = low, .id = OrderId{10}},
            OrderReject{.reason = RejectReason::InvalidPrice, .seq = high, .id = OrderId{20}},
            OrderReject{.reason = RejectReason::InvalidPrice, .seq = ioc, .id = OrderId{30}},
        };
        EXPECT_EQ(this->listener.events, expected);
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, PricesOneTickInsideTheExtremesAreAccepted) {
        const Price lowest {min_price.value + 1};
        const Price highest {max_price.value - 1};
        this->send(buy(OrderId{10}, lowest, Quantity{5}));
        this->send(sell(OrderId{20}, highest, Quantity{5}));

        EXPECT_TRUE(this->order_rejects().empty());
        EXPECT_EQ(this->book.best_bid(), lowest);
        EXPECT_EQ(this->book.best_ask(), highest);
    }

    TYPED_TEST(RulesTest, MarketOrderPriceIsNotValidated) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        // A market order's price field is ignored, so even INT64_MAX there is not a reject.
        this->send(new_order(Side::Buy, OrderId{20}, max_price, Quantity{5}, TimeInForce::Market));

        EXPECT_TRUE(this->order_rejects().empty());
        ASSERT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->trades()[0].price, Price{50});
    }

    TYPED_TEST(RulesTest, DuplicateOfARestingIdIsRejectedAndTheRestingOrderIsUntouched) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        SeqNo dup = this->send(sell(OrderId{10}, Price{60}, Quantity{3}));

        std::vector<OrderReject> expected {
            OrderReject{.reason = RejectReason::DuplicateId, .seq = dup, .id = OrderId{10}},
        };
        EXPECT_EQ(this->order_rejects(), expected);
        EXPECT_EQ(this->order_acks().size(), 1u);  // only the original order's ack
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Buy), 5u);

        // The id still belongs to the original order: cancelling it removes the buy at 50.
        SeqNo cancelled = this->send(cancel(OrderId{10}));
        std::vector<CancelAck> acks {
            CancelAck{.side = Side::Buy, .qty = Quantity{5}, .seq = cancelled, .id = OrderId{10}, .price = Price{50}},
        };
        EXPECT_EQ(this->cancel_acks(), acks);
    }

    TYPED_TEST(RulesTest, DuplicateIdIsRejectedBeforeItCanTrade) {
        this->send(sell(OrderId{10}, Price{50}, Quantity{5}));
        // Same id, crossing price: without the check this would trade the order against itself.
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        // An IOC never rests, but its id must still not clash with a live order.
        this->send(ioc_buy(OrderId{10}, Price{50}, Quantity{5}));

        EXPECT_EQ(this->order_rejects().size(), 2u);
        EXPECT_TRUE(this->trades().empty());
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Sell), 5u);
    }

    TYPED_TEST(RulesTest, RejectedOrderNeverRestsSoItsIdCannotBeCancelled) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{0}));
        SeqNo cancelled = this->send(cancel(OrderId{10}));

        std::vector<CancelReject> expected {CancelReject{.seq = cancelled, .id = OrderId{10}}};
        EXPECT_EQ(this->cancel_rejects(), expected);
    }

    TYPED_TEST(RulesTest, FirstFailingCheckIsTheReasonReported) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        // Zero qty AND invalid price AND duplicate id: zero quantity is checked first.
        SeqNo all_three = this->send(sell(OrderId{10}, max_price, Quantity{0}));
        // Invalid price AND duplicate id: price is checked before the id.
        SeqNo two = this->send(sell(OrderId{10}, min_price, Quantity{5}));

        std::vector<OrderReject> expected {
            OrderReject{.reason = RejectReason::ZeroQuantity, .seq = all_three, .id = OrderId{10}},
            OrderReject{.reason = RejectReason::InvalidPrice, .seq = two, .id = OrderId{10}},
        };
        EXPECT_EQ(this->order_rejects(), expected);
    }

    TYPED_TEST(RulesTest, ModifyToZeroQuantityIsRejectedAndKeepsTimePriority) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(buy(OrderId{20}, Price{50}, Quantity{5}));
        SeqNo modified = this->send(modify(OrderId{10}, Side::Buy, Price{50}, Quantity{0}));

        std::vector<ModifyReject> expected {ModifyReject{.seq = modified, .id = OrderId{10}}};
        EXPECT_EQ(this->modify_rejects(), expected);
        EXPECT_TRUE(this->modify_acks().empty());
        EXPECT_EQ(this->book.depth_at(Price{50}, Side::Buy), 10u);

        // Order 10 is still first in the queue: a rejected modify must not cancel-and-reinsert.
        this->send(sell(OrderId{30}, Price{50}, Quantity{5}));
        ASSERT_EQ(this->trades().size(), 1u);
        EXPECT_EQ(this->trades()[0].maker_id, OrderId{10});
    }

    TYPED_TEST(RulesTest, ModifyToInt64MinOrMaxPriceIsRejectedAndChangesNothing) {
        this->send(buy(OrderId{10}, Price{50}, Quantity{5}));
        this->send(sell(OrderId{20}, Price{60}, Quantity{5}));
        // Repricing the buy to INT64_MAX would also cross the ask: it must be rejected, not traded.
        SeqNo high = this->send(modify(OrderId{10}, Side::Buy, max_price, Quantity{5}));
        SeqNo low = this->send(modify(OrderId{20}, Side::Sell, min_price, Quantity{5}));

        std::vector<ModifyReject> expected {
            ModifyReject{.seq = high, .id = OrderId{10}},
            ModifyReject{.seq = low, .id = OrderId{20}},
        };
        EXPECT_EQ(this->modify_rejects(), expected);
        EXPECT_TRUE(this->trades().empty());
        EXPECT_EQ(this->book.best_bid(), Price{50});
        EXPECT_EQ(this->book.best_ask(), Price{60});
    }

    // ---- Order ids: every uint64_t is a valid id ----
    //
    // Nothing reserves an id value, so 0 and the maximum must rest, fill, cancel and modify like
    // any other id. (A hash table that marks empty buckets with a reserved key, often 0, breaks this.)

    TYPED_TEST(RulesTest, IdsAtTheEdgesOfTheRangeRestFillAndCancel) {
        constexpr OrderId zero {0};
        constexpr OrderId max {std::numeric_limits<std::uint64_t>::max()};
        constexpr OrderId max_minus_one {std::numeric_limits<std::uint64_t>::max() - 1};

        this->send(buy(zero, Price{100}, Quantity{5}));
        this->send(buy(max, Price{100}, Quantity{5}));
        this->send(buy(max_minus_one, Price{99}, Quantity{5}));
        EXPECT_TRUE(this->order_rejects().empty()) << "no id is a duplicate of an empty slot";

        // A duplicate of each edge id is still a duplicate.
        this->send(buy(zero, Price{98}, Quantity{1}));
        this->send(buy(max, Price{98}, Quantity{1}));
        ASSERT_EQ(this->order_rejects().size(), 2u);
        EXPECT_EQ(this->order_rejects()[0].reason, RejectReason::DuplicateId);
        EXPECT_EQ(this->order_rejects()[1].reason, RejectReason::DuplicateId);

        // Fill id 0 completely (it is first in time at 100), then cancel the max id.
        const SeqNo hit = this->send(sell(OrderId{10}, Price{100}, Quantity{5}));
        const SeqNo cxl = this->send(cancel(max));
        const std::vector<Trade> expected_trades {
            Trade{.qty = Quantity{5}, .seq = hit, .maker_id = zero, .taker_id = OrderId{10}, .price = Price{100}},
        };
        EXPECT_EQ(this->trades(), expected_trades);
        const std::vector<CancelAck> expected_cancels {
            CancelAck{.side = Side::Buy, .qty = Quantity{5}, .seq = cxl, .id = max, .price = Price{100}},
        };
        EXPECT_EQ(this->cancel_acks(), expected_cancels);

        // Both are gone now; the third edge id is still live and can be modified.
        this->send(cancel(zero));
        this->send(cancel(max));
        EXPECT_EQ(this->cancel_rejects().size(), 2u);
        this->send(modify(max_minus_one, Side::Buy, Price{97}, Quantity{3}));
        EXPECT_EQ(this->modify_acks().size(), 1u);
        EXPECT_EQ(this->book.best_bid(), Price{97});
        EXPECT_EQ(this->book.depth_at(Price{97}, Side::Buy), 3u);

        // And the freed ids can be used again.
        this->send(buy(zero, Price{96}, Quantity{1}));
        this->send(buy(max, Price{96}, Quantity{1}));
        EXPECT_EQ(this->order_rejects().size(), 2u);
        EXPECT_EQ(this->book.depth_at(Price{96}, Side::Buy), 2u);
    }

    // ---- depth_at, run once per side ----

    template <typename BookT, Side s>
    struct OnSide {
        using Book = BookT;
        static constexpr Side side = s;
    };

    template <typename T>
    class SidedRulesTest : public BookTest<typename T::Book> {
    protected:
        static constexpr Side side = T::side;
    };

    struct BaselineBuy : OnSide<BaselineBook<RecordingListener>, Side::Buy> {};
    struct BaselineSell : OnSide<BaselineBook<RecordingListener>, Side::Sell> {};
    struct FastBuy : OnSide<FastBook<RecordingListener>, Side::Buy> {};
    struct FastSell : OnSide<FastBook<RecordingListener>, Side::Sell> {};

    using SidedBookTypes = ::testing::Types<BaselineBuy, BaselineSell, FastBuy, FastSell>;

    TYPED_TEST_SUITE(SidedRulesTest, SidedBookTypes);

    TYPED_TEST(SidedRulesTest, DepthAtIsZeroOnEmptyBook) {
        EXPECT_EQ(this->book.depth_at(Price{0}, this->side), 0);
    }

    TYPED_TEST(SidedRulesTest, DepthAtIsZeroForMissingPriceLevel) {
        this->send(new_order(this->side, OrderId{1}, Price{0}, Quantity{5}));
        this->send(new_order(this->side, OrderId{2}, Price{1}, Quantity{5}));

        EXPECT_EQ(this->book.depth_at(Price{2}, this->side), 0);
    }

    TYPED_TEST(SidedRulesTest, DepthAtIsSumOfQuantitiesAtPriceLevel) {
        this->send(new_order(this->side, OrderId{1}, Price{0}, Quantity{5}));
        this->send(new_order(this->side, OrderId{2}, Price{0}, Quantity{10}));
        this->send(new_order(this->side, OrderId{3}, Price{0}, Quantity{15}));

        EXPECT_EQ(this->book.depth_at(Price{0}, this->side), 5 + 10 + 15);
    }

    TYPED_TEST(SidedRulesTest, DepthAtIgnoresOtherPriceLevels) {
        this->send(new_order(this->side, OrderId{1}, Price{0}, Quantity{5}));

        this->send(new_order(this->side, OrderId{2}, Price{1}, Quantity{10}));
        this->send(new_order(this->side, OrderId{3}, Price{2}, Quantity{15}));

        EXPECT_EQ(this->book.depth_at(Price{1}, this->side), 10);
    }

    TYPED_TEST(SidedRulesTest, DepthAtIgnoresOppositeSide) {
        this->send(new_order(this->side, OrderId{1}, Price{5}, Quantity{5}));

        Side other_side = opposite(this->side);

        // Rest the opposite side away from ours (bids below asks) so the book never crosses
        Price other_price = this->side == Side::Buy ? Price{10} : Price{0};

        this->send(new_order(other_side, OrderId{2}, other_price, Quantity{5}));
        this->send(new_order(other_side, OrderId{3}, other_price, Quantity{10}));

        EXPECT_EQ(this->book.depth_at(other_price, this->side), 0);
    }

    TYPED_TEST(SidedRulesTest, DepthAtCanGoPastUINT32MAX) {
        Quantity quantity {std::numeric_limits<std::uint32_t>::max()};
        this->send(new_order(this->side, OrderId{1}, Price{0}, quantity));
        this->send(new_order(this->side, OrderId{2}, Price{0}, quantity));

        // Widen before multiplying: 2u * UINT32_MAX is computed in 32 bits and wraps to 4294967294.
        EXPECT_EQ(this->book.depth_at(Price{0}, this->side), std::uint64_t{2} * UINT32_MAX);
    }

    // ---- depth_at after the book changes, run once per side ----
    //
    // These pin down every way a level's quantity can change, so a cached per-level total (v2
    // step 1) is checked after each one: a fill (partial and full), a sweep through several levels,
    // a cancel, a modify (to another level, to the same level, after a partial fill), a level that
    // empties and is created again, and the cases that must NOT change depth (rejects, IOC and
    // market remainders). `this->side` is the side resting in the book; the aggressor is opposite.

    // A price k ticks behind p on side s, i.e. further from the touch: lower for bids, higher for asks.
    inline Price behind(Side s, Price p, std::int64_t k) {
        return Price{s == Side::Buy ? p.value - k : p.value + k};
    }

    TYPED_TEST(SidedRulesTest, DepthDropsByTheFillOnAPartialFillOfTheMaker) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{10}));
        this->send(new_order(opposite(s), OrderId{20}, Price{100}, Quantity{4}));

        EXPECT_EQ(this->book.depth_at(Price{100}, s), 6u);
    }

    TYPED_TEST(SidedRulesTest, DepthKeepsTheRestOfTheLevelWhenTheFrontMakerIsFullyFilled) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{5}));
        this->send(new_order(s, OrderId{20}, Price{100}, Quantity{7}));
        this->send(new_order(opposite(s), OrderId{30}, Price{100}, Quantity{5}));

        EXPECT_EQ(this->book.depth_at(Price{100}, s), 7u);
    }

    TYPED_TEST(SidedRulesTest, DepthIsZeroAfterTheLevelIsFilledAway) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{5}));
        this->send(new_order(opposite(s), OrderId{20}, Price{100}, Quantity{5}));

        EXPECT_EQ(this->book.depth_at(Price{100}, s), 0u);
    }

    TYPED_TEST(SidedRulesTest, DepthOfEachLevelDropsInASweepThroughSeveralLevels) {
        const Side s = this->side;
        const Price p0 {100};
        const Price p1 = behind(s, p0, 1);
        const Price p2 = behind(s, p0, 2);
        this->send(new_order(s, OrderId{10}, p0, Quantity{5}));
        this->send(new_order(s, OrderId{20}, p1, Quantity{5}));
        this->send(new_order(s, OrderId{30}, p2, Quantity{5}));

        // Limit price p1: takes all of p0 and 3 of p1, and stops before p2.
        this->send(new_order(opposite(s), OrderId{40}, p1, Quantity{8}));

        EXPECT_EQ(this->book.depth_at(p0, s), 0u);
        EXPECT_EQ(this->book.depth_at(p1, s), 2u);
        EXPECT_EQ(this->book.depth_at(p2, s), 5u);
    }

    TYPED_TEST(SidedRulesTest, DepthOfEachLevelDropsInAMarketOrderSweep) {
        const Side s = this->side;
        const Price p0 {100};
        const Price p1 = behind(s, p0, 1);
        this->send(new_order(s, OrderId{10}, p0, Quantity{5}));
        this->send(new_order(s, OrderId{20}, p1, Quantity{5}));

        this->send(new_order(opposite(s), OrderId{30}, Price{0}, Quantity{7}, TimeInForce::Market));

        EXPECT_EQ(this->book.depth_at(p0, s), 0u);
        EXPECT_EQ(this->book.depth_at(p1, s), 3u);
    }

    TYPED_TEST(SidedRulesTest, AnAggressorThatRestsAddsOnlyItsUnfilledRemainder) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{3}));
        this->send(new_order(opposite(s), OrderId{20}, Price{100}, Quantity{10}));

        EXPECT_EQ(this->book.depth_at(Price{100}, opposite(s)), 7u);
        EXPECT_EQ(this->book.depth_at(Price{100}, s), 0u);
    }

    TYPED_TEST(SidedRulesTest, AnIocRemainderAddsNoDepth) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{3}));
        this->send(new_order(opposite(s), OrderId{20}, Price{100}, Quantity{10}, TimeInForce::IOC));

        EXPECT_EQ(this->book.depth_at(Price{100}, opposite(s)), 0u);
    }

    TYPED_TEST(SidedRulesTest, CancelAfterAPartialFillRemovesOnlyWhatWasLeft) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{10}));
        this->send(new_order(s, OrderId{20}, Price{100}, Quantity{5}));
        this->send(new_order(opposite(s), OrderId{30}, Price{100}, Quantity{4}));  // id 10 has 6 left

        this->send(cancel(OrderId{10}));

        EXPECT_EQ(this->book.depth_at(Price{100}, s), 5u);
    }

    TYPED_TEST(SidedRulesTest, ModifyToAnotherPriceMovesTheQuantityBetweenLevels) {
        const Side s = this->side;
        const Price p0 {100};
        const Price p1 = behind(s, p0, 1);
        this->send(new_order(s, OrderId{10}, p0, Quantity{5}));
        this->send(new_order(s, OrderId{20}, p0, Quantity{7}));

        this->send(modify(OrderId{10}, s, p1, Quantity{9}));

        EXPECT_EQ(this->book.depth_at(p0, s), 7u);
        EXPECT_EQ(this->book.depth_at(p1, s), 9u);
    }

    TYPED_TEST(SidedRulesTest, ModifyAtTheSamePriceReplacesTheQuantity) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{5}));
        this->send(new_order(s, OrderId{20}, Price{100}, Quantity{7}));

        this->send(modify(OrderId{10}, s, Price{100}, Quantity{2}));

        EXPECT_EQ(this->book.depth_at(Price{100}, s), 9u);
    }

    TYPED_TEST(SidedRulesTest, ModifyAfterAPartialFillReplacesWhatWasLeft) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{10}));
        this->send(new_order(opposite(s), OrderId{20}, Price{100}, Quantity{4}));  // id 10 has 6 left

        this->send(modify(OrderId{10}, s, Price{100}, Quantity{8}));

        EXPECT_EQ(this->book.depth_at(Price{100}, s), 8u);
    }

    TYPED_TEST(SidedRulesTest, RejectedModifiesLeaveDepthUnchanged) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{5}));

        this->send(modify(OrderId{10}, opposite(s), Price{100}, Quantity{9}));  // wrong side
        this->send(modify(OrderId{10}, s, Price{100}, Quantity{0}));            // zero quantity
        this->send(modify(OrderId{99}, s, Price{100}, Quantity{9}));            // unknown id

        EXPECT_EQ(this->modify_rejects().size(), 3u);
        EXPECT_EQ(this->book.depth_at(Price{100}, s), 5u);
    }

    TYPED_TEST(SidedRulesTest, ARejectedDuplicateIdAddsNoDepth) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{5}));
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{7}));

        EXPECT_EQ(this->order_rejects().size(), 1u);
        EXPECT_EQ(this->book.depth_at(Price{100}, s), 5u);
    }

    TYPED_TEST(SidedRulesTest, ALevelCreatedAgainAfterACancelEmptiedItStartsFromZero) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{5}));
        this->send(cancel(OrderId{10}));
        this->send(new_order(s, OrderId{20}, Price{100}, Quantity{3}));

        EXPECT_EQ(this->book.depth_at(Price{100}, s), 3u);
    }

    TYPED_TEST(SidedRulesTest, ALevelCreatedAgainAfterBeingFilledAwayStartsFromZero) {
        const Side s = this->side;
        this->send(new_order(s, OrderId{10}, Price{100}, Quantity{5}));
        this->send(new_order(opposite(s), OrderId{20}, Price{100}, Quantity{5}));
        this->send(new_order(s, OrderId{30}, Price{100}, Quantity{3}));

        EXPECT_EQ(this->book.depth_at(Price{100}, s), 3u);
    }
}
