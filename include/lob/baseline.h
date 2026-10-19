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
            return get_best(bids);
        }

        [[nodiscard]] std::optional<Price> best_ask() const {
            return get_best(asks);
        }

        void apply(const Command &command) {
            NewOrder new_order {std::get<NewOrder>(command)};

            OrderId id {new_order.id};
            Side side {new_order.side};
            Price price {new_order.price};
            Quantity qty {new_order.qty};

            Order order = {.id = id, .qty = qty};

            if (side == Side::Buy) {
                add_to(bids, price, order);
            } else {
                add_to(asks, price, order);
            }
        }
    private:
        struct Order {
            OrderId id;
            Quantity qty;
        };

        std::map<Price, std::list<Order>, std::greater<Price>> bids;
        std::map<Price, std::list<Order>> asks;

        [[nodiscard]] std::optional<Price> get_best(const auto& book_side) const {
            auto first_entry {book_side.begin()};
            if (first_entry == book_side.end()) {
                return std::nullopt;
            }

            return first_entry->first;
        }

        void add_to(auto& book_side, Price price, Order order) {
            auto [level_it, _] = book_side.try_emplace(price);
            auto& queue {level_it->second};
            queue.push_back(order);
        }

    };
}


#endif //ORDERBOOK_BASELINE_H
