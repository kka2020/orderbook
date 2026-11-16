// v2 step 5: SortedLevels, the sorted vector of price levels that replaces each FastBook side's
// std::map<Price, Level, Compare>.
//
// The interface these tests force (include/lob/sorted_levels.h):
//
//   template <std::default_initializable Value, class Compare> class SortedLevels {
//       Value& find_or_insert(Price price);           // a value-initialised Value if price is new
//       Value* find(Price price) noexcept;             // nullptr if absent
//       const Value* find(Price price) const noexcept;
//       bool erase(Price price) noexcept;              // false if absent
//       Price best_price() const noexcept;             // precondition: !empty()
//       Value& best() noexcept;                        // precondition: !empty()
//       void pop_best() noexcept;                      // precondition: !empty()
//       std::size_t size() const noexcept;
//       bool empty() const noexcept;
//   };
//
// Compare{}(a, b) is true when price a has priority over price b, as in std::map: bids use
// std::greater<Price> (highest first) and asks std::less<Price> (lowest first). "Best" is the
// price with top priority. Default-constructible, nothrow-movable.
//
// A reference or pointer into the container is valid only until the next find_or_insert, erase or
// pop_best: those can shift or reallocate the vector.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "lob/sorted_levels.h"

namespace lob::test {
    namespace {
        using Bids = SortedLevels<std::uint64_t, std::greater<Price>>;
        using Asks = SortedLevels<std::uint64_t, std::less<Price>>;

        static_assert(std::is_default_constructible_v<Bids>);
        static_assert(std::is_nothrow_move_constructible_v<Bids>,
                      "FastBook must stay nothrow-movable, so its levels must be too");

        constexpr std::int64_t kMin {std::numeric_limits<std::int64_t>::min()};
        constexpr std::int64_t kMax {std::numeric_limits<std::int64_t>::max()};

        // A value tied to its price, so a value that ends up under the wrong price is caught.
        std::uint64_t tag(Price p) {
            return static_cast<std::uint64_t>(p.value) * 7 + 3;
        }

        // Typed over both sides: every behaviour must hold for bids and asks.
        template <class Levels>
        class SortedLevelsTest : public ::testing::Test {
        protected:
            using Compare = std::conditional_t<std::is_same_v<Levels, Bids>, std::greater<Price>, std::less<Price>>;

            // True when a has priority over b on this side.
            static bool better(Price a, Price b) { return Compare{}(a, b); }

            // The best of some prices on this side.
            static Price best_of(std::vector<Price> prices) {
                return *std::min_element(prices.begin(), prices.end(), [](Price a, Price b) { return better(a, b); });
            }

            void insert_tagged(std::initializer_list<std::int64_t> prices) {
                for (std::int64_t p : prices) {
                    levels.find_or_insert(Price{p}) = tag(Price{p});
                }
            }

            // Pops every level and returns the prices in the order they came out, checking that each
            // value is still the one tagged to its price.
            std::vector<Price> drain() {
                std::vector<Price> order;
                while (!levels.empty()) {
                    const Price p {levels.best_price()};
                    EXPECT_EQ(levels.best(), tag(p)) << "value under price " << p.value;
                    order.push_back(p);
                    levels.pop_best();
                }
                return order;
            }

            Levels levels;
        };

        using BothSides = ::testing::Types<Bids, Asks>;
        TYPED_TEST_SUITE(SortedLevelsTest, BothSides);

        // ---- Basic behaviour ----

        TYPED_TEST(SortedLevelsTest, NewContainerIsEmpty) {
            const auto& const_levels = this->levels;
            EXPECT_TRUE(this->levels.empty());
            EXPECT_EQ(this->levels.size(), 0u);
            EXPECT_EQ(this->levels.find(Price{100}), nullptr);
            EXPECT_EQ(const_levels.find(Price{100}), nullptr);
            EXPECT_FALSE(this->levels.erase(Price{100}));
        }

        TYPED_TEST(SortedLevelsTest, FindOrInsertCreatesAValueInitialisedLevelOnce) {
            std::uint64_t& first {this->levels.find_or_insert(Price{100})};
            EXPECT_EQ(first, 0u) << "a new level starts value-initialised";
            first = 42;

            EXPECT_EQ(this->levels.find_or_insert(Price{100}), 42u) << "an existing level is returned, not replaced";
            EXPECT_EQ(this->levels.size(), 1u);
            EXPECT_FALSE(this->levels.empty());

            const auto& const_levels = this->levels;
            ASSERT_NE(const_levels.find(Price{100}), nullptr);
            EXPECT_EQ(*const_levels.find(Price{100}), 42u);
        }

        TYPED_TEST(SortedLevelsTest, FindReturnsALevelThatCanBeChangedInPlace) {
            this->insert_tagged({100, 101});
            std::uint64_t* level {this->levels.find(Price{101})};
            ASSERT_NE(level, nullptr);
            *level = 5;
            EXPECT_EQ(this->levels.find_or_insert(Price{101}), 5u);
            ASSERT_NE(this->levels.find(Price{100}), nullptr);
            EXPECT_EQ(*this->levels.find(Price{100}), tag(Price{100}));
        }

        // A level erased and created again must not keep the old value (e.g. a reused slot that
        // was never reset).
        TYPED_TEST(SortedLevelsTest, ALevelCreatedAgainStartsFresh) {
            this->levels.find_or_insert(Price{100}) = 42;
            ASSERT_TRUE(this->levels.erase(Price{100}));
            EXPECT_EQ(this->levels.find_or_insert(Price{100}), 0u) << "after erase";

            this->levels.find_or_insert(Price{100}) = 43;
            this->levels.pop_best();
            EXPECT_EQ(this->levels.find_or_insert(Price{100}), 0u) << "after pop_best";
        }

        // ---- Priority order ----

        TYPED_TEST(SortedLevelsTest, BestIsTheHighestBidOrTheLowestAsk) {
            this->insert_tagged({100, 102, 101, 99});
            const Price expected {this->best_of({Price{100}, Price{102}, Price{101}, Price{99}})};
            EXPECT_EQ(this->levels.best_price(), expected);
            EXPECT_EQ(this->levels.best(), tag(expected));
        }

        // A new level can land at either end or in the middle; only a better price changes the best.
        TYPED_TEST(SortedLevelsTest, OnlyABetterPriceBecomesTheNewBest) {
            this->insert_tagged({100});
            const Price better_price {this->better(Price{101}, Price{100}) ? Price{101} : Price{99}};
            const Price worse_price {this->better(Price{101}, Price{100}) ? Price{99} : Price{101}};

            this->levels.find_or_insert(worse_price) = tag(worse_price);
            EXPECT_EQ(this->levels.best_price(), Price{100}) << "a worse level goes behind the best";

            this->levels.find_or_insert(better_price) = tag(better_price);
            EXPECT_EQ(this->levels.best_price(), better_price) << "a better level becomes the best";

            this->levels.find_or_insert(Price{100}) = tag(Price{100});  // existing: no change
            EXPECT_EQ(this->levels.best_price(), better_price);
            EXPECT_EQ(this->levels.size(), 3u);
        }

        TYPED_TEST(SortedLevelsTest, PopBestVisitsLevelsInPriorityOrder) {
            std::vector<std::int64_t> prices;
            for (std::int64_t p = -300; p <= 300; p += 3) {
                prices.push_back(p);
            }
            std::mt19937_64 rng {7};
            std::shuffle(prices.begin(), prices.end(), rng);
            for (std::int64_t p : prices) {
                this->levels.find_or_insert(Price{p}) = tag(Price{p});
            }
            ASSERT_EQ(this->levels.size(), prices.size());

            const std::vector<Price> order {this->drain()};
            ASSERT_EQ(order.size(), prices.size());
            for (std::size_t i = 1; i < order.size(); ++i) {
                EXPECT_TRUE(this->better(order[i - 1], order[i]))
                    << order[i - 1].value << " came out before " << order[i].value;
            }
        }

        // The full range of valid prices. INT64_MIN and INT64_MAX themselves are invalid prices (the
        // book rejects them), but their neighbours are valid. Comparing prices by subtracting them
        // would overflow here.
        TYPED_TEST(SortedLevelsTest, ExtremePricesAreOrderedCorrectly) {
            this->insert_tagged({0, kMax - 1, -1, kMin + 1, 1});
            const std::vector<Price> order {this->drain()};
            const std::vector<Price> ascending {Price{kMin + 1}, Price{-1}, Price{0}, Price{1}, Price{kMax - 1}};
            std::vector<Price> expected {ascending};
            if (this->better(Price{1}, Price{0})) {
                std::reverse(expected.begin(), expected.end());
            }
            EXPECT_EQ(order, expected);
        }

        // ---- Erase ----

        TYPED_TEST(SortedLevelsTest, EraseRemovesOnlyThatLevel) {
            this->insert_tagged({100, 101, 102, 103, 104});
            const Price best {this->levels.best_price()};
            const Price worst {this->better(Price{100}, Price{104}) ? Price{104} : Price{100}};

            // Middle, then worst, then best.
            for (const Price victim : {Price{102}, worst, best}) {
                SCOPED_TRACE(testing::Message() << "erasing " << victim.value);
                EXPECT_TRUE(this->levels.erase(victim));
                EXPECT_FALSE(this->levels.erase(victim)) << "a second erase must report absent";
                EXPECT_EQ(this->levels.find(victim), nullptr);
            }

            EXPECT_EQ(this->levels.size(), 2u);
            for (const Price kept : {Price{101}, Price{103}}) {
                ASSERT_NE(this->levels.find(kept), nullptr) << kept.value;
                EXPECT_EQ(*this->levels.find(kept), tag(kept));
            }
            EXPECT_EQ(this->levels.best_price(), this->best_of({Price{101}, Price{103}}));
        }

        TYPED_TEST(SortedLevelsTest, ErasingTheBestMakesTheNextBestTheBest) {
            this->insert_tagged({100, 101, 102});
            const Price first {this->levels.best_price()};
            ASSERT_TRUE(this->levels.erase(first));
            EXPECT_EQ(this->levels.best_price(), Price{101});  // the middle price on either side
            EXPECT_EQ(this->levels.best(), tag(Price{101}));
        }

        TYPED_TEST(SortedLevelsTest, EraseOfAMissingPriceBetweenLevelsChangesNothing) {
            this->insert_tagged({100, 102});
            EXPECT_FALSE(this->levels.erase(Price{101}));
            EXPECT_FALSE(this->levels.erase(Price{99}));
            EXPECT_FALSE(this->levels.erase(Price{103}));
            EXPECT_EQ(this->levels.size(), 2u);
            EXPECT_EQ(this->levels.find(Price{101}), nullptr);
        }

        TYPED_TEST(SortedLevelsTest, MovedContainerKeepsItsLevels) {
            this->insert_tagged({100, 105, 95});
            auto moved {std::move(this->levels)};
            EXPECT_EQ(moved.size(), 3u);
            for (const Price p : {Price{95}, Price{100}, Price{105}}) {
                ASSERT_NE(moved.find(p), nullptr);
                EXPECT_EQ(*moved.find(p), tag(p));
            }
        }

        // ---- Random churn against std::map ----
        //
        // Mixed find_or_insert / erase / pop_best / find on a small price range (so most operations
        // hit live levels), plus a few extreme prices. After every operation the size and the best
        // level must match a std::map with the same Compare; every 500 operations every price is
        // checked.
        template <class Levels, class Compare>
        void churn_against_map(std::uint64_t seed, int ops) {
            Levels levels;
            std::map<Price, std::uint64_t, Compare> reference;
            std::mt19937_64 rng {seed};

            const std::vector<Price> extremes {Price{kMin + 1}, Price{kMax - 1}, Price{-1}, Price{0}};
            constexpr std::int64_t lo {90}, hi {130};
            auto pick_price = [&]() -> Price {
                if (rng() % 100 == 0) {
                    return extremes[rng() % extremes.size()];
                }
                return Price{lo + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(hi - lo + 1))};
            };

            for (int i = 0; i < ops; ++i) {
                const std::uint64_t roll {rng() % 20};
                if (roll < 9) {  // add to a level, creating it if needed
                    const Price p {pick_price()};
                    const std::uint64_t add {rng() % 1000 + 1};
                    levels.find_or_insert(p) += add;
                    reference[p] += add;
                } else if (roll < 15) {  // erase a level (it may be absent)
                    const Price p {pick_price()};
                    ASSERT_EQ(levels.erase(p), reference.erase(p) == 1) << "erase " << p.value << " at op " << i;
                } else if (roll < 17) {  // pop the best level
                    if (!reference.empty()) {
                        ASSERT_FALSE(levels.empty()) << "at op " << i;
                        levels.pop_best();
                        reference.erase(reference.begin());
                    }
                } else {  // look up a level
                    const Price p {pick_price()};
                    const auto it {reference.find(p)};
                    const std::uint64_t* value {levels.find(p)};
                    if (it == reference.end()) {
                        ASSERT_EQ(value, nullptr) << "find " << p.value << " at op " << i;
                    } else {
                        ASSERT_NE(value, nullptr) << "find " << p.value << " at op " << i;
                        ASSERT_EQ(*value, it->second) << "find " << p.value << " at op " << i;
                    }
                }

                ASSERT_EQ(levels.size(), reference.size()) << "at op " << i;
                ASSERT_EQ(levels.empty(), reference.empty()) << "at op " << i;
                if (!reference.empty()) {
                    ASSERT_EQ(levels.best_price(), reference.begin()->first) << "at op " << i;
                    ASSERT_EQ(levels.best(), reference.begin()->second) << "at op " << i;
                }

                if (i % 500 == 499) {
                    for (std::int64_t p = lo - 1; p <= hi + 1; ++p) {
                        ASSERT_EQ(levels.find(Price{p}) != nullptr, reference.contains(Price{p}))
                            << "price " << p << " after op " << i;
                    }
                    for (const Price p : extremes) {
                        ASSERT_EQ(levels.find(p) != nullptr, reference.contains(p))
                            << "price " << p.value << " after op " << i;
                    }
                }
            }
        }

        TEST(SortedLevelsChurn, BidsMatchStdMap) {
            for (std::uint64_t seed = 1; seed <= 3; ++seed) {
                SCOPED_TRACE(testing::Message() << "seed " << seed);
                churn_against_map<Bids, std::greater<Price>>(seed, 50'000);
            }
        }

        TEST(SortedLevelsChurn, AsksMatchStdMap) {
            for (std::uint64_t seed = 4; seed <= 6; ++seed) {
                SCOPED_TRACE(testing::Message() << "seed " << seed);
                churn_against_map<Asks, std::less<Price>>(seed, 50'000);
            }
        }
    }
}
