#ifndef ORDERBOOK_BOOK_TEST_SUPPORT_H
#define ORDERBOOK_BOOK_TEST_SUPPORT_H
#include "lob/commands.h"
#include "lob/types.h"

namespace lob::test {
    inline NewOrder buy(OrderId id, Price price, Quantity qty) {
        return NewOrder {
            .side = Side::Buy,
            .tif = TimeInForce::GTC,
            .qty = qty,
            .seq = SeqNo{1},
            .id = id,
            .price = price,
        };
    }

    inline NewOrder sell(OrderId id, Price price, Quantity qty) {
        return NewOrder {
            .side = Side::Sell,
            .tif = TimeInForce::GTC,
            .qty = qty,
            .seq = SeqNo{1},
            .id = id,
            .price = price,
        };
    }
}

#endif //ORDERBOOK_BOOK_TEST_SUPPORT_H
