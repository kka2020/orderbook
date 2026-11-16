#ifndef ORDERBOOK_FLAT_ID_MAP_H
#define ORDERBOOK_FLAT_ID_MAP_H
#include <cassert>
#include <cstddef>
#include <vector>
#include <optional>
#include <bit>
#include <limits>
#include <concepts>

#include "types.h"

namespace lob {
    struct OrderIdHash {
        std::size_t operator()(OrderId id) const noexcept {
            static_assert(sizeof(std::size_t) == 8);

            std::size_t z {id.value};
            // Use splitmix64 finalizer to effectively mix the bits up
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            return z ^ (z >> 31);
        }
    };

    template <std::default_initializable Value, typename Hash = OrderIdHash>
    class FlatIdMap {
    public:
        explicit FlatIdMap(std::size_t capacity) : size_(0) {
            assert(capacity < std::numeric_limits<std::size_t>::max() / kMaxLoadFactorInverse);
            std::size_t needed {capacity * kMaxLoadFactorInverse};

            table_.resize(std::bit_ceil(needed));
        }

        bool insert(OrderId id, const Value& value) {
            bool will_overload {(size_ + 1) * kMaxLoadFactorInverse > table_.size()};
            bool result {linear_probe_insert(id, value)};

            if (result) {
                if (will_overload) {
                    resize();
                }

                size_++;
            }

            return result;
        }

        Value *find(OrderId id) noexcept {
            std::optional<std::size_t> idx {linear_probe_find(id)};
            return idx.has_value() ? &(table_[*idx].value) : nullptr;
        }

        [[nodiscard]] bool contains(OrderId id) const noexcept {
            return linear_probe_find(id).has_value();
        }

        bool erase(OrderId id) noexcept {
            std::optional<std::size_t> found_idx {linear_probe_find(id)};

            if (!found_idx) {
                return false;
            }

            backwards_shift(*found_idx);
            size_--;
            return true;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }

        [[nodiscard]] bool empty() const noexcept {
            return size_ == 0;
        }
    private:
        struct FlatIdMapEntry {
            OrderId id {0};
            Value value {};
            bool occupied {false};
        };

        bool linear_probe_insert(OrderId id, Value value, std::vector<FlatIdMapEntry>& table) noexcept {
            std::size_t idx {wrap_around(Hash{}(id), table.size())};
            // Only way to get into infinite loop is if all entries are occupied, which is impossible since
            // resize would have been triggered before then
            while (table[idx].occupied && table[idx].id != id) {
                idx = wrap_around(idx + 1, table.size());
            }

            if (table[idx].id == id && table[idx].occupied) {
                return false;
            }

            table[idx] = FlatIdMapEntry{.id = id, .value = value, .occupied = true};
            return true;
        }

        bool linear_probe_insert(OrderId id, Value value) noexcept {
            return linear_probe_insert(id, value, table_);
        }

        [[nodiscard]] std::optional<std::size_t> linear_probe_find(OrderId id) const noexcept {
            /* Returns index of entry with that id, otherwise nullopt */
            std::size_t idx {wrap_around(Hash{}(id))};
            while (table_[idx].occupied) {
                if (table_[idx].id == id) {
                    return idx;
                }
                idx = wrap_around(idx + 1);
            }

            return std::nullopt;
        }

        void resize() {
            std::vector<FlatIdMapEntry> new_table(table_.size() * 2);

            for (auto& [id, value, occupied] : table_) {
                if (!occupied) {
                    continue;
                }

                linear_probe_insert(id, value, new_table);
            }

            table_ = std::move(new_table);
        }

        [[nodiscard]] std::size_t wrap_around(std::size_t idx, std::size_t table_size) const noexcept {
            return (idx) & (table_size - 1);
        }

        [[nodiscard]] std::size_t wrap_around(std::size_t idx) const noexcept {
            return wrap_around(idx, table_.size());
        }

        [[nodiscard]] std::size_t dist(std::size_t a, std::size_t b) const noexcept {
            return (b - a) & (table_.size() - 1);
        }

        void backwards_shift(std::size_t idx) noexcept {
            std::size_t gap {idx}, curr {wrap_around(idx + 1)};
            while (table_[curr].occupied) {
                std::size_t home_idx {wrap_around(Hash{}(table_[curr].id))};

                if (dist(home_idx, curr) >= dist(gap, curr)) {
                    table_[gap] = table_[curr];
                    gap = curr;
                }
                curr = wrap_around(curr + 1);
            }
            table_[gap].occupied = false;
        }

        std::vector<FlatIdMapEntry> table_;
        std::size_t size_;
        static constexpr std::size_t kMaxLoadFactorInverse {2};
    };
}

#endif //ORDERBOOK_FLAT_ID_MAP_H
