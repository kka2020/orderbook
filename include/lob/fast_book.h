#ifndef ORDERBOOK_FAST_BOOK_H
#define ORDERBOOK_FAST_BOOK_H

#include <list>
#include <optional>
#include <map>
#include <functional>
#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <cstddef>
#include <cstdint>

#include "policies.h"
#include "concepts.h"
#include "types.h"
#include "commands.h"
#include "events.h"

namespace lob {
    template <EventListener Listener, typename MatchPolicy = FifoPolicy>
    class FastBook {
    public:
        explicit FastBook(Listener& l, std::size_t capacity = 1 << 16) : listener(l) {
            id_table.reserve(capacity);
        }

        FastBook(const FastBook&) = delete;
        FastBook& operator=(const FastBook&) = delete;
        FastBook(FastBook&&) noexcept = default;

        [[nodiscard]] std::optional<Price> best_bid() const noexcept {
            return bids.get_best();
        }

        [[nodiscard]] std::optional<Price> best_ask() const noexcept {
            return asks.get_best();
        }

        [[nodiscard]] std::uint64_t depth_at(Price price, Side side) const noexcept {
            if (side == Side::Buy) {
                return bids.depth_at(price);
            }

            return asks.depth_at(price);
        }

        void apply(const Command &command) {
            std::visit([this](const auto &c) { handle(c); }, command);
        }
    private:
        struct Order {
            OrderId id;
            Quantity qty;
        };

        using OrderQueue = std::list<Order>;
        static_assert(MatchPolicyFor<MatchPolicy, OrderQueue>,
             "MatchPolicy must provide static Quantity fill_level(Level&, Quantity, on_fill) for this book's Level");

        struct Level {
            OrderQueue orders;
            std::uint64_t total {0};
        };

        using OrderIt = OrderQueue::iterator;

        struct OrderIdHash {
            std::size_t operator()(OrderId id) const noexcept {
                return std::hash<std::uint64_t>{}(id.value);
            }
        };

        struct IdTableEntry {
            Side side;
            Price price;
            OrderIt it;
        };

        using IdTable = std::unordered_map<OrderId, IdTableEntry, OrderIdHash>;

        template <Side S>
        class BookSide {
        public:
            [[nodiscard]] std::optional<Price> get_best() const {
                auto first_entry {levels.begin()};
                if (first_entry == levels.end()) {
                    return std::nullopt;
                }

                return first_entry->first;
            }

            OrderIt add(Price price, Order order) {
                auto [level_it, _] = levels.try_emplace(price);

                auto& [queue, total] {level_it->second};
                auto order_it = queue.insert(queue.end(), order);
                total += order.qty.value;

                return order_it;
            }

            [[nodiscard]] std::uint64_t depth_at(Price price) const {
                auto level_it {levels.find(price)};
                if (level_it == levels.end()) {
                    return 0;
                }

                return level_it->second.total;
            }

            [[nodiscard]] Quantity cross(const NewOrder& taker, Listener& l, IdTable& id_table_ref) {
                Quantity remaining_qty {taker.qty};
                // Check that book side is non-empty, the taker has quantity left, and that the taker's price still
                // crosses the book if it's not a market order
                while (!levels.empty() && (is_market(taker.tif) || crosses(taker.price)) && remaining_qty.value > 0) {
                    // We already check that levels is non-empty, so this is safe
                    auto level_it {levels.begin()};
                    auto& [price, level] = *level_it;

                    remaining_qty = MatchPolicy::fill_level(level.orders, remaining_qty, [&](Order& maker, Quantity fill_qty) {
                        l.on(Trade{
                            .qty = fill_qty,
                            .seq = taker.seq,
                            .maker_id = maker.id,
                            .taker_id = taker.id,
                            .price = price,
                        });

                        level.total -= fill_qty.value;

                        if (maker.qty.value == 0) {
                            id_table_ref.erase(maker.id);
                        }
                    });

                    // If level is now empty, delete it
                    if (level.orders.empty()) {
                        levels.erase(level_it);
                    }
                }

                return remaining_qty;
            }

            void cancel_order(Price price, OrderIt it) {
                // PRE: levels.contains(price) && it is valid
                auto level_it {levels.find(price)};
                auto& level {level_it->second};

                // Reduce level.total before iterator is deleted
                level.total -= it->qty.value;

                level.orders.erase(it);
                if (level.orders.empty()) {
                    levels.erase(level_it);
                }
            }
        private:
            [[nodiscard]] bool crosses(Price price) const {
                std::optional best {get_best()};
                return best.has_value() && !Compare{}(price, *best);
            }

            using Compare = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;
            std::map<Price, Level, Compare> levels;
        };

        struct MatchResult {
            Quantity remaining_qty;
        };

        std::optional<RejectReason> validate(const NewOrder &new_order) {
            std::optional<RejectReason> reject_reason {};

            // Quantity check
            if (new_order.qty == Quantity{0}) {
                reject_reason.emplace(RejectReason::ZeroQuantity);
            // Price check
            } else if (!is_market(new_order.tif) && is_invalid_price(new_order.price)) {
                reject_reason.emplace(RejectReason::InvalidPrice);
            // Duplicate-id check
            } else if (id_table.contains(new_order.id)) {
                reject_reason.emplace(RejectReason::DuplicateId);
            }

            return reject_reason;
        }

        MatchResult match(const NewOrder& new_order) {
            // Cross the opposite side
            Quantity remaining_qty {
                new_order.side == Side::Buy ?
                    asks.cross(new_order, listener, id_table) :
                    bids.cross(new_order, listener, id_table)
            };

            return MatchResult { .remaining_qty = remaining_qty };
        }

        void after_match(const NewOrder& new_order, MatchResult result) {
            // If there is not remaining quantity, we don't need to rest an order or cancel
            if (result.remaining_qty.value == 0) {
                return;
            }

            // If the order is IOC or Market, we cancel the order for the remaining quantity
            if (!rests(new_order.tif)) {
                listener.on(CancelAck{
                    .side = new_order.side,
                    .qty = result.remaining_qty,
                    .seq = new_order.seq,
                    .id = new_order.id,
                    .price = new_order.price,
                });
                return;
            }

            // Otherwise, we proceed to rest the order
            Order order = {.id = new_order.id, .qty = result.remaining_qty};

            OrderIt order_it {
                new_order.side == Side::Buy ? bids.add(new_order.price, order) : asks.add(new_order.price, order)
            };

            // Add this order to the id table
            id_table.emplace(new_order.id, IdTableEntry{
                .side = new_order.side, .price = new_order.price, .it = order_it
            });
        }

        void handle(const NewOrder& new_order) {
            // If the order isn't valid, emit OrderReject with the stored rejection reason and early return
            if (std::optional<RejectReason> reject_reason {validate(new_order)}) {
                listener.on(OrderReject{
                    .reason = *reject_reason,
                    .seq = new_order.seq,
                    .id = new_order.id,
                });
                return;
            }

            listener.on(OrderAck{
                .side = new_order.side,
                .qty = new_order.qty,
                .seq = new_order.seq,
                .id = new_order.id,
                .price = new_order.price
            });

            MatchResult result = match(new_order);
            after_match(new_order, result);
        }

        Quantity remove_resting(IdTable::iterator id_table_it) {
            auto [side, price, it] = id_table_it->second;
            // Read qty now: 'it' is invalid once the order is erased. Returned so the caller can report it
            Quantity qty {it->qty};

            // Erase order from the book
            if (side == Side::Buy) {
                bids.cancel_order(price, it);
            } else {
                asks.cancel_order(price, it);
            }

            id_table.erase(id_table_it);
            return qty;
        }

        void handle(const Cancel& cancel) {
            // If the id lookup table doesn't contain the id we're trying to cancel, emit a rejection event and return
            auto id_table_it = id_table.find(cancel.id);
            if (id_table_it == id_table.end()) {
                listener.on(CancelReject{
                    .seq = cancel.seq,
                    .id = cancel.id,
                });
                return;
            }

            // The id is resting. Copy side and price for the CancelAck now, because remove_resting erases the entry
            auto [side, price, _] = id_table_it->second;

            Quantity qty = remove_resting(id_table_it);

            // Emit CancelAck event
            listener.on(CancelAck{
                .side = side,
                .qty = qty,
                .seq = cancel.seq,
                .id = cancel.id,
                .price = price
            });
        }

        bool rejects_modify(const Modify &modify, IdTable::iterator id_table_it) {
            return id_table_it == id_table.end() || id_table_it->second.side != modify.side
                   || modify.new_quantity == Quantity{0} || is_invalid_price(modify.new_price);
        }

        void handle(const Modify &modify) {
            auto id_table_it {id_table.find(modify.id)};

            // Check if modify request is valid, otherwise emit ModifyReject and early exit
            if (rejects_modify(modify, id_table_it)) {
                listener.on(ModifyReject{
                    .seq = modify.seq,
                    .id = modify.id,
                });

                return;
            }

            Side side = id_table_it->second.side;
            remove_resting(id_table_it);

            NewOrder new_order {
                .side = side,
                .tif = TimeInForce::GTC,
                .qty = modify.new_quantity,
                .seq = modify.seq,
                .id = modify.id,
                .price = modify.new_price,
            };

            MatchResult result {match(new_order)};
            after_match(new_order, result);

            listener.on(ModifyAck{
                .side = side,
                .qty = modify.new_quantity,
                .seq = modify.seq,
                .id = modify.id,
                .price = modify.new_price,
            });
        }

        Listener& listener;
        BookSide<Side::Buy> bids;
        BookSide<Side::Sell> asks;
        IdTable id_table;
    };
}

#endif //ORDERBOOK_FAST_BOOK_H
