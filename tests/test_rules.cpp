#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "book_test_support.h"
#include "lob/baseline.h"

namespace lob::test {
    template <typename Book>
    class RulesTest : public BookTest<Book> {};

    using BookTypes = ::testing::Types<BaselineBook<RecordingListener>>;
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

    using SidedBookTypes = ::testing::Types<BaselineBuy, BaselineSell>;

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
