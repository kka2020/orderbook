#ifndef ORDERBOOK_EVENTS_H
#define ORDERBOOK_EVENTS_H
#include <type_traits>

#include "types.h"

namespace lob {
    // Emitted once for every accepted new order, before any Trade it causes.
    // Carries the order as submitted (original price and qty).
    struct OrderAck {
        Side side;
        Quantity qty;
        SeqNo seq;
        OrderId id;
        Price price;

        constexpr bool operator==(const OrderAck&) const noexcept = default;
    };

    static_assert(sizeof(OrderAck) == 32);
    static_assert(std::is_trivially_copyable_v<OrderAck>);

    // One fill between a resting order (maker) and the incoming order (taker).
    // price is the maker's price; seq is the taker's (aggressor's) sequence number.
    struct Trade {
        Quantity qty;
        SeqNo seq;
        OrderId maker_id;
        OrderId taker_id;
        Price price;

        constexpr bool operator==(const Trade&) const noexcept = default;
    };

    static_assert(sizeof(Trade) == 40);
    static_assert(std::is_trivially_copyable_v<Trade>);

    // Quantity removed from the book or from an incoming order without trading.
    // For an IOC or market order: emitted after its Trades, qty = the unfilled remainder,
    // other fields copied from the order as submitted.
    struct CancelAck {
        Side side;
        Quantity qty;
        SeqNo seq;
        OrderId id;
        Price price;

        constexpr bool operator==(const CancelAck&) const noexcept = default;
    };

    static_assert(sizeof(CancelAck) == 32);
    static_assert(std::is_trivially_copyable_v<CancelAck>);
}

#endif //ORDERBOOK_EVENTS_H
