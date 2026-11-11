#ifndef ORDERBOOK_BASELINE_H
#define ORDERBOOK_BASELINE_H
#include <list>
#include <optional>
#include <map>
#include <functional>
#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <variant>

#include "types.h"
#include "commands.h"
#include "events.h"

namespace lob {

    template <typename Listener>
    class BaselineBook {
    public:
        explicit BaselineBook(Listener& l) : listener {l} {}

        [[nodiscard]] std::optional<Price> best_bid() const {
            return bids.get_best();
        }

        [[nodiscard]] std::optional<Price> best_ask() const {
            return asks.get_best();
        }

        [[nodiscard]] std::uint64_t depth_at(Price price, Side side) const {
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
        using OrderIt = OrderQueue::iterator;

        struct OrderIdHash {
            std::size_t operator()(OrderId id) const noexcept {
                return std::hash<std::uint64_t>{}(id.value);
            }
        };

        struct CancelTableEntry {
            Side side;
            Price price;
            OrderIt it;
        };

        using CancelTable = std::unordered_map<OrderId, CancelTableEntry, OrderIdHash>;

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
                auto& queue {level_it->second};
                auto order_it = queue.insert(queue.end(), order);
                return order_it;
            }

            [[nodiscard]] std::uint64_t depth_at(Price price) const {
                auto level_it {levels.find(price)};
                if (level_it == levels.end()) {
                    return 0;
                }

                const auto& orders {level_it->second};
                std::uint64_t sum {0};
                for (const auto& order : orders) {
                    sum += order.qty.value;
                }

                return sum;
            }

            [[nodiscard]] Quantity cross(const NewOrder& taker, Listener& l, CancelTable& cancel_table_ref) {
                Quantity remaining_qty {taker.qty};
                // Check that book side is non-empty, the taker has quantity left, and that the taker's price still
                // crosses the book if it's not a market order
                while (!levels.empty() && (is_market(taker.tif) || crosses(taker.price)) && remaining_qty.value > 0) {
                    // We already check that levels is non-empty, so this is safe
                    auto level {levels.begin()};
                    auto& [price, best_orders] = *level;

                    // Consume orders until level runs out or taker runs out of qty
                    while (!best_orders.empty() && remaining_qty.value > 0) {
                        Order& maker {best_orders.front()};

                        Quantity fill_size {std::min(maker.qty, remaining_qty)};

                        maker.qty -= fill_size;
                        remaining_qty -= fill_size;

                        l.on(Trade{
                            .qty = fill_size,
                            .seq = taker.seq,
                            .maker_id = maker.id,
                            .taker_id = taker.id,
                            .price = price,
                        });

                        if (maker.qty.value == 0) {
                            cancel_table_ref.erase(maker.id);
                            best_orders.pop_front();
                        }
                    }

                    // If level is now empty, delete it
                    if (best_orders.empty()) {
                        levels.erase(level);
                    }
                }

                return remaining_qty;
            }

            void cancel_order(Price price, OrderIt it) {
                // PRE: levels.contains(price) && it is valid
                auto level_it {levels.find(price)};
                auto& level {level_it->second};

                level.erase(it);
                if (level.empty()) {
                    levels.erase(level_it);
                }
            }
        private:
            [[nodiscard]] bool crosses(Price price) const {
                std::optional best {get_best()};
                return best.has_value() && !Compare{}(price, *best);
            }

            using Compare = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;
            std::map<Price, OrderQueue, Compare> levels;
        };

        void cross_and_rest(const NewOrder &new_order) {
            // Cross the opposite side
            Quantity remaining_qty {
                new_order.side == Side::Buy ?
                    asks.cross(new_order, listener, cancel_table) :
                    bids.cross(new_order, listener, cancel_table)
            };

            // If there is not remaining quantity, we don't need to rest an order or cancel
            if (remaining_qty.value == 0) {
                return;
            }

            // If the order is IOC or Market, we cancel the order for the remaining quantity
            if (!rests(new_order.tif)) {
                listener.on(CancelAck{
                    .side = new_order.side,
                    .qty = remaining_qty,
                    .seq = new_order.seq,
                    .id = new_order.id,
                    .price = new_order.price,
                });
                return;
            }

            // Otherwise, we proceed to rest the order
            Order order = {.id = new_order.id, .qty = remaining_qty};

            OrderIt order_it {
                new_order.side == Side::Buy ? bids.add(new_order.price, order) : asks.add(new_order.price, order)
            };

            // Add this order to the cancel table
            cancel_table.emplace(new_order.id, CancelTableEntry{
                .side = new_order.side, .price = new_order.price, .it = order_it
            });
        }

        void handle(const NewOrder &new_order) {
            listener.on(OrderAck{
                .side = new_order.side,
                .qty = new_order.qty,
                .seq = new_order.seq,
                .id = new_order.id,
                .price = new_order.price
            });

            cross_and_rest(new_order);
        }

        Quantity remove_resting(CancelTable::iterator cancel_table_it) {
            auto [side, price, it] = cancel_table_it->second;
            // Read qty now: 'it' is invalid once the order is erased. Returned so the caller can report it
            Quantity qty {it->qty};

            // Erase order from the book
            if (side == Side::Buy) {
                bids.cancel_order(price, it);
            } else {
                asks.cancel_order(price, it);
            }

            cancel_table.erase(cancel_table_it);
            return qty;
        }

        void handle(const Cancel &cancel) {
            // If the id lookup table doesn't contain the id we're trying to cancel, emit a rejection event and return
            auto cancel_table_it = cancel_table.find(cancel.id);
            if (cancel_table_it == cancel_table.end()) {
                listener.on(CancelReject{
                    .seq = cancel.seq,
                    .id = cancel.id,
                });
                return;
            }

            // The id is resting. Copy side and price for the CancelAck now, because remove_resting erases the entry
            auto [side, price, _] = cancel_table_it->second;

            Quantity qty = remove_resting(cancel_table_it);

            // Emit CancelAck event
            listener.on(CancelAck{
                .side = side,
                .qty = qty,
                .seq = cancel.seq,
                .id = cancel.id,
                .price = price
            });
        }

        void handle(const Modify& modify) {
            auto cancel_table_it {cancel_table.find(modify.id)};
            if (cancel_table_it == cancel_table.end() || cancel_table_it->second.side != modify.side) {
                listener.on(ModifyReject{
                    .seq = modify.seq,
                    .id = modify.id,
                });
                return;
            }

            Side side = cancel_table_it->second.side;
            remove_resting(cancel_table_it);

            NewOrder new_order {
                .side = side,
                .tif = TimeInForce::GTC,
                .qty = modify.new_quantity,
                .seq = modify.seq,
                .id = modify.id,
                .price = modify.new_price,
            };
            cross_and_rest(new_order);

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
        CancelTable cancel_table;
    };
}


#endif //ORDERBOOK_BASELINE_H
