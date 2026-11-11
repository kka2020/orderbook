#ifndef ORDERBOOK_EVENTS_H
#define ORDERBOOK_EVENTS_H
#include <cstdint>
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

    // Why a new order was rejected. When several apply, the first in this order is reported.
    enum class RejectReason : std::uint8_t {
        ZeroQuantity,  // qty == 0 (any time in force)
        InvalidPrice,  // GTC or IOC with price INT64_MIN or INT64_MAX (a market order's price is ignored)
        DuplicateId,   // the id belongs to an order resting right now (reuse after a fill or cancel is fine)
    };

    // Emitted INSTEAD of OrderAck for a new order that is not accepted. Nothing else is emitted for
    // it and the book is unchanged. seq and id are the rejected order's.
    struct OrderReject {
        RejectReason reason;
        SeqNo seq;
        OrderId id;

        constexpr bool operator==(const OrderReject&) const noexcept = default;
    };

    static_assert(sizeof(OrderReject) == 24);
    static_assert(std::is_trivially_copyable_v<OrderReject>);

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
    // For a Cancel of a resting order: seq = the Cancel's seq, qty = what was still resting,
    // side and price = the resting order's.
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

    // A Cancel for an id that is not resting right now: never seen, already fully filled,
    // already cancelled, or an IOC/market order that never rested. The book is unchanged.
    struct CancelReject {
        SeqNo seq;
        OrderId id;

        constexpr bool operator==(const CancelReject&) const noexcept = default;
    };

    static_assert(sizeof(CancelReject) == 16);
    static_assert(std::is_trivially_copyable_v<CancelReject>);

    // A Modify of a resting order succeeded. Emitted after any Trades the reinsert caused.
    // seq = the Modify's seq; qty and price = the new values requested (qty is the new open
    // quantity, not added to what was left); side = the order's side.
    struct ModifyAck {
        Side side;
        Quantity qty;
        SeqNo seq;
        OrderId id;
        Price price;

        constexpr bool operator==(const ModifyAck&) const noexcept = default;
    };

    static_assert(sizeof(ModifyAck) == 32);
    static_assert(std::is_trivially_copyable_v<ModifyAck>);

    // A Modify that could not be applied: the id is not resting right now, the Modify's side
    // differs from the resting order's side, the new quantity is 0, or the new price is
    // INT64_MIN or INT64_MAX. The book is unchanged: a rejected modify leaves the order resting
    // exactly as it was, with its time priority.
    struct ModifyReject {
        SeqNo seq;
        OrderId id;

        constexpr bool operator==(const ModifyReject&) const noexcept = default;
    };

    static_assert(sizeof(ModifyReject) == 16);
    static_assert(std::is_trivially_copyable_v<ModifyReject>);
}

#endif //ORDERBOOK_EVENTS_H
