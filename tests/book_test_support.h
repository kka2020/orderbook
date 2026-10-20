#ifndef ORDERBOOK_BOOK_TEST_SUPPORT_H
#define ORDERBOOK_BOOK_TEST_SUPPORT_H
#include "lob/commands.h"
#include "lob/types.h"

namespace lob::test {

    inline NewOrder new_order(Side side, OrderId id, Price price, Quantity qty) {
        return NewOrder {
            .side = side,
            .tif = TimeInForce::GTC,
            .qty = qty,
            .seq = SeqNo{1},
            .id = id,
            .price = price,
        };
    }

    inline NewOrder buy(OrderId id, Price price, Quantity qty) {
        return new_order(Side::Buy, id, price, qty);
    }

    inline NewOrder sell(OrderId id, Price price, Quantity qty) {
        return new_order(Side::Sell, id, price, qty);
    }

    inline Side opposite(Side side) {
        return side == Side::Buy ? Side::Sell : Side::Buy;
    }
}

#endif //ORDERBOOK_BOOK_TEST_SUPPORT_H
