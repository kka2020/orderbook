#include <cstdint>
#include <limits>
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
}
