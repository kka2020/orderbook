#ifndef ORDERBOOK_CONCEPTS_H
#define ORDERBOOK_CONCEPTS_H
#include <optional>
#include <concepts>
#include <cstdint>

#include "commands.h"
#include "events.h"
#include "types.h"

namespace lob {
    template <typename B>
    concept OrderBookLike = requires(const B& b_const, B& b_mod, Price p, Side s, const Command& c) {
        { b_const.best_bid() } noexcept -> std::same_as<std::optional<Price>>;
        { b_const.best_ask() } noexcept -> std::same_as<std::optional<Price>>;
        { b_const.depth_at(p, s) } noexcept -> std::same_as<std::uint64_t>;
        b_mod.apply(c);
    };

    template <typename L>
    concept EventListener = requires(L& l,
        const Trade& trade,
        const OrderAck& order_ack,
        const CancelAck& cancel_ack,
        const CancelReject& cancel_reject,
        const ModifyAck& modify_ack,
        const ModifyReject& modify_reject) {
        l.on(trade);
        l.on(order_ack);
        l.on(cancel_ack);
        l.on(cancel_reject);
        l.on(modify_ack);
        l.on(modify_reject);
    };
}

#endif //ORDERBOOK_CONCEPTS_H
