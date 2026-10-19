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

}
