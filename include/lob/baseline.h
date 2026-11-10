#ifndef ORDERBOOK_BASELINE_H
#define ORDERBOOK_BASELINE_H
#include <list>
#include <optional>
#include <map>
#include <functional>
#include <algorithm>
#include <type_traits>

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
            NewOrder new_order {std::get<NewOrder>(command)};

            listener.on(OrderAck{
                .side = new_order.side,
                .qty = new_order.qty,
                .seq = new_order.seq,
                .id = new_order.id,
                .price = new_order.price
            });

            // Cross the opposite side
            Quantity remaining_qty {
                new_order.side == Side::Buy ? asks.cross(new_order, listener) : bids.cross(new_order, listener)
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
            if (new_order.side == Side::Buy) {
                bids.add(new_order.price, order);
            } else {
                asks.add(new_order.price, order);
            }
        }
    private:
        struct Order {
            OrderId id;
            Quantity qty;
        };

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

            void add(Price price, Order order) {
                auto [level_it, _] = levels.try_emplace(price);
                auto& queue {level_it->second};
                queue.push_back(order);
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

            [[nodiscard]] Quantity cross(const NewOrder& taker, Listener& l) {
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
        private:
            [[nodiscard]] bool crosses(Price price) const {
                std::optional best {get_best()};
                return best.has_value() && !Compare{}(price, *best);
            }

            using Compare = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;
            std::map<Price, std::list<Order>, Compare> levels;
        };

        Listener& listener;
        BookSide<Side::Buy> bids;
        BookSide<Side::Sell> asks;
    };
}


#endif //ORDERBOOK_BASELINE_H
