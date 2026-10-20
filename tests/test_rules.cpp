#include <gtest/gtest.h>

#include "book_test_support.h"
#include "lob/baseline.h"

namespace lob::test {
    template <typename Book>
    class RulesTest : public  ::testing::Test {
    protected:
        Book book;
    };

    using BookTypes = ::testing::Types<BaselineBook>;
    TYPED_TEST_SUITE(RulesTest, BookTypes);

    TYPED_TEST(RulesTest, BestBidIsNullOptWhenNoRestingBuys) {
        EXPECT_EQ(this->book.best_bid(), std::nullopt);
    }

    TYPED_TEST(RulesTest, BestBidIsHighestPriceAmongRestingBuys) {
        this->book.apply(buy(OrderId{2}, Price{2}, Quantity{5}));
        this->book.apply(buy(OrderId{1}, Price{3}, Quantity{5}));
        this->book.apply(buy(OrderId{3}, Price{1}, Quantity{5}));

        EXPECT_EQ(this->book.best_bid(), Price{3});
    }

    TYPED_TEST(RulesTest, BestAskIsNullOptWhenNoRestingSells) {
        EXPECT_EQ(this->book.best_ask(), std::nullopt);
    }

    TYPED_TEST(RulesTest, BestAskIsLowestPriceAmongRestingSells) {
        this->book.apply(sell(OrderId{2}, Price{2}, Quantity{5}));
        this->book.apply(sell(OrderId{3}, Price{1}, Quantity{5}));
        this->book.apply(sell(OrderId{1}, Price{3}, Quantity{5}));

        EXPECT_EQ(this->book.best_ask(), Price{1});
    }

    template <typename BookT, Side s>
    struct OnSide {
        using Book = BookT;
        static constexpr Side side = s;
    };

    template <typename T>
    class SidedRulesTest : public ::testing::Test {
    protected:
        T::Book book;
        static constexpr Side side = T::side;
    };

    struct BaselineBuy : OnSide<BaselineBook, Side::Buy> {};
    struct BaselineSell : OnSide<BaselineBook, Side::Sell> {};

    using SidedBookTypes = ::testing::Types<BaselineBuy,BaselineSell>;

    TYPED_TEST_SUITE(SidedRulesTest, SidedBookTypes);

    TYPED_TEST(SidedRulesTest, DepthAtIsZeroForEmptyPriceLevel) {
        EXPECT_EQ(this->book.depth_at(Price{0}, this->side), 0);
    }

    TYPED_TEST(SidedRulesTest, DepthAtIsSumOfQuantitiesAtPriceLevel) {
        this->book.apply(new_order(this->side, OrderId{1}, Price{0}, Quantity{5}));
        this->book.apply(new_order(this->side, OrderId{2}, Price{0}, Quantity{10}));
        this->book.apply(new_order(this->side, OrderId{3}, Price{0}, Quantity{15}));

        EXPECT_EQ(this->book.depth_at(Price{0}, this->side), 5 + 10 + 15);
    }

    TYPED_TEST(SidedRulesTest, DepthAtIgnoresOtherPriceLevels) {
        this->book.apply(new_order(this->side, OrderId{1}, Price{0}, Quantity{5}));

        this->book.apply(new_order(this->side, OrderId{2}, Price{1}, Quantity{5}));
        this->book.apply(new_order(this->side, OrderId{3}, Price{1}, Quantity{10}));

        EXPECT_EQ(this->book.depth_at(Price{0}, this->side), 5);
    }

    TYPED_TEST(SidedRulesTest, DepthAtIgnoresOppositeSide) {
        this->book.apply(new_order(this->side, OrderId{1}, Price{5}, Quantity{5}));

        Side other_side = opposite(this->side);

        // Rest the opposite side away from ours (bids below asks) so the book never crosses
        Price other_price = this->side == Side::Buy ? Price{10} : Price{0};

        this->book.apply(new_order(other_side, OrderId{2}, other_price, Quantity{5}));
        this->book.apply(new_order(other_side, OrderId{3}, other_price, Quantity{10}));

        EXPECT_EQ(this->book.depth_at(other_price, this->side), 0);
    }
}
