//
// Created by kavin on 18/10/2026.
//

#ifndef ORDERBOOK_BASELINE_H
#define ORDERBOOK_BASELINE_H
#include <list>
#include <optional>
#include <map>
#include <functional>

#include "types.h"
#include "commands.h"

namespace lob {

    class BaselineBook {
    public:
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

            OrderId id {new_order.id};
            Side side {new_order.side};
            Price price {new_order.price};
            Quantity qty {new_order.qty};

            Order order = {.id = id, .qty = qty};

            if (side == Side::Buy) {
                bids.add(price, order);
            } else {
                asks.add(price, order);
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
        private:
            using Compare = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;
            std::map<Price, std::list<Order>, Compare> levels;
        };

        BookSide<Side::Buy> bids;
        BookSide<Side::Sell> asks;
    };
}


#endif //ORDERBOOK_BASELINE_H
