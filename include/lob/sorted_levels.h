#ifndef ORDERBOOK_SORTED_LEVELS_H
#define ORDERBOOK_SORTED_LEVELS_H
#include <cassert>
#include <concepts>
#include <algorithm>
#include <vector>
#include <utility>

#include "types.h"

namespace lob {
    template <std::default_initializable Value, typename Compare>
    class SortedLevels {
    public:
        Value& find_or_insert(Price price) {
            auto it {(bin_search_it(levels_, price))};

            if (it != levels_.end() && it->price == price) {
                return it->value;
            }

            auto inserted = levels_.insert(it, Entry{.price = price, .value = Value{}});
            return inserted->value;
        }

        const Value* find(Price price) const noexcept {
            auto it {bin_search_it(levels_, price)};
            return (it != levels_.end() && it->price == price) ? &it->value : nullptr;
        }

        Value* find(Price price) noexcept {
            return const_cast<Value*>(std::as_const(*this).find(price));
        }

        bool erase(Price price) noexcept {
            auto it {bin_search_it(levels_, price)};

            if (it == levels_.end() || it->price != price) {
                return false;
            }

            levels_.erase(it);
            return true;
        }

        [[nodiscard]] Price best_price() const noexcept {
            assert(!empty());
            return levels_.back().price;
        }

        Value& best() noexcept {
            assert(!empty());
            return levels_.back().value;
        }

        void pop_best() noexcept {
            assert(!empty());
            levels_.pop_back();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return levels_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return levels_.empty();
        }
    private:
        struct Entry {
            Price price {0};
            Value value {};
        };

        static auto bin_search_it(auto& levels, Price price) {
            auto reverse_compare = [](Price left, Price right) -> auto { return Compare{}(right, left); };
            return std::ranges::lower_bound(levels, price, reverse_compare, &Entry::price);
        }

        std::vector<Entry> levels_;
    };
}

#endif //ORDERBOOK_SORTED_LEVELS_H
