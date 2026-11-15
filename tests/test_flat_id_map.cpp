// v2 step 4: FlatIdMap, the open-addressing table that replaces FastBook's std::unordered_map
// from order id to resting-order locator.
//
// The interface these tests force (include/lob/flat_id_map.h):
//
//   template <class Value, class Hash = OrderIdHash> class FlatIdMap {
//       explicit FlatIdMap(std::size_t capacity);    // capacity is a hint: the map grows past it
//       bool insert(OrderId id, const Value& value);  // false (and no change) if id is present
//       Value* find(OrderId id) noexcept;             // nullptr if absent
//       bool contains(OrderId id) const noexcept;
//       bool erase(OrderId id) noexcept;              // false if id was absent
//       std::size_t size() const noexcept;
//       bool empty() const noexcept;
//   };
//
// Hash is a template parameter so these tests can force collisions: a real hash spreads keys out,
// so the clustering and wrap-around cases below would otherwise almost never happen. Hash must be
// callable as `std::size_t Hash{}(OrderId)`.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "lob/flat_id_map.h"

namespace lob::test {
    namespace {
        constexpr std::uint64_t kMaxId {std::numeric_limits<std::uint64_t>::max()};

        // Every key hashes to the same value, so all keys form one cluster and every lookup
        // has to probe past the others.
        struct CollideAll {
            std::size_t operator()(OrderId) const noexcept { return 0; }
        };

        // Even ids hash to the largest size_t, odd ids to 0. A table that picks a bucket with
        // `hash & (buckets - 1)` homes even ids in the LAST bucket and odd ids in the FIRST, so one
        // cluster wraps around the end of the table and mixes keys from both homes. Deleting from
        // it is where "is this key's home between the gap and here?" has to wrap correctly: a key
        // homed in bucket 0 must never be moved back into the last bucket.
        struct WrapAround {
            std::size_t operator()(OrderId id) const noexcept {
                return id.value % 2 == 0 ? std::numeric_limits<std::size_t>::max() : 0;
            }
        };

        // Seven home buckets: long clusters that run into each other.
        struct SevenBuckets {
            std::size_t operator()(OrderId id) const noexcept { return static_cast<std::size_t>(id.value % 7); }
        };

        static_assert(std::is_nothrow_move_constructible_v<FlatIdMap<std::uint64_t>>,
                      "FastBook must stay nothrow-movable, so its id map must be too");

        // ---- Basic behaviour, with the default hash ----

        TEST(FlatIdMap, NewMapIsEmpty) {
            FlatIdMap<std::uint64_t> map {16};
            EXPECT_TRUE(map.empty());
            EXPECT_EQ(map.size(), 0u);
            EXPECT_EQ(map.find(OrderId{10}), nullptr);
            EXPECT_FALSE(map.contains(OrderId{10}));
            EXPECT_FALSE(map.erase(OrderId{10}));
        }

        TEST(FlatIdMap, InsertedValueCanBeFoundAndChangedInPlace) {
            FlatIdMap<std::uint64_t> map {16};
            EXPECT_TRUE(map.insert(OrderId{10}, 111));
            EXPECT_EQ(map.size(), 1u);
            EXPECT_FALSE(map.empty());
            EXPECT_TRUE(map.contains(OrderId{10}));

            std::uint64_t* value {map.find(OrderId{10})};
            ASSERT_NE(value, nullptr);
            EXPECT_EQ(*value, 111u);

            *value = 222;
            ASSERT_NE(map.find(OrderId{10}), nullptr);
            EXPECT_EQ(*map.find(OrderId{10}), 222u);
        }

        TEST(FlatIdMap, DuplicateInsertFailsAndKeepsTheOriginalValue) {
            FlatIdMap<std::uint64_t> map {16};
            ASSERT_TRUE(map.insert(OrderId{10}, 111));
            EXPECT_FALSE(map.insert(OrderId{10}, 999));
            EXPECT_EQ(map.size(), 1u);
            ASSERT_NE(map.find(OrderId{10}), nullptr);
            EXPECT_EQ(*map.find(OrderId{10}), 111u);
        }

        TEST(FlatIdMap, EraseRemovesOnlyThatId) {
            FlatIdMap<std::uint64_t> map {16};
            ASSERT_TRUE(map.insert(OrderId{10}, 111));
            ASSERT_TRUE(map.insert(OrderId{20}, 222));

            EXPECT_TRUE(map.erase(OrderId{10}));
            EXPECT_FALSE(map.erase(OrderId{10})) << "a second erase of the same id must report absent";
            EXPECT_EQ(map.size(), 1u);
            EXPECT_EQ(map.find(OrderId{10}), nullptr);
            ASSERT_NE(map.find(OrderId{20}), nullptr);
            EXPECT_EQ(*map.find(OrderId{20}), 222u);
        }

        TEST(FlatIdMap, ErasedIdCanBeInsertedAgain) {
            FlatIdMap<std::uint64_t> map {16};
            ASSERT_TRUE(map.insert(OrderId{10}, 111));
            ASSERT_TRUE(map.erase(OrderId{10}));
            EXPECT_TRUE(map.insert(OrderId{10}, 333));
            ASSERT_NE(map.find(OrderId{10}), nullptr);
            EXPECT_EQ(*map.find(OrderId{10}), 333u);
        }

        // Order ids are any uint64_t: the book accepts 0 and the maximum. An open-addressing table
        // that marks empty buckets with a reserved key (0 is the usual choice) would treat one of
        // these ids as "empty".
        TEST(FlatIdMap, IdsAtTheEdgesOfTheRangeAreOrdinaryKeys) {
            FlatIdMap<std::uint64_t> map {16};
            EXPECT_FALSE(map.contains(OrderId{0}));
            EXPECT_FALSE(map.contains(OrderId{kMaxId}));
            EXPECT_EQ(map.find(OrderId{0}), nullptr);
            EXPECT_EQ(map.find(OrderId{kMaxId}), nullptr);
            EXPECT_FALSE(map.erase(OrderId{0}));
            EXPECT_FALSE(map.erase(OrderId{kMaxId}));

            ASSERT_TRUE(map.insert(OrderId{0}, 1));
            ASSERT_TRUE(map.insert(OrderId{kMaxId}, 2));
            ASSERT_TRUE(map.insert(OrderId{kMaxId - 1}, 3));
            EXPECT_EQ(map.size(), 3u);
            ASSERT_NE(map.find(OrderId{0}), nullptr);
            ASSERT_NE(map.find(OrderId{kMaxId}), nullptr);
            ASSERT_NE(map.find(OrderId{kMaxId - 1}), nullptr);
            EXPECT_EQ(*map.find(OrderId{0}), 1u);
            EXPECT_EQ(*map.find(OrderId{kMaxId}), 2u);
            EXPECT_EQ(*map.find(OrderId{kMaxId - 1}), 3u);

            EXPECT_TRUE(map.erase(OrderId{0}));
            EXPECT_FALSE(map.contains(OrderId{0}));
            EXPECT_TRUE(map.contains(OrderId{kMaxId}));
            EXPECT_TRUE(map.erase(OrderId{kMaxId}));
            EXPECT_FALSE(map.contains(OrderId{kMaxId}));
            EXPECT_TRUE(map.contains(OrderId{kMaxId - 1}));
        }

        // Capacity is a hint, like FastBook's: inserting past it grows the table and keeps every
        // entry. Capacity 0 and 1 make it grow from the very first inserts.
        TEST(FlatIdMap, GrowsPastItsCapacityAndKeepsEveryEntry) {
            for (std::size_t capacity : {std::size_t{0}, std::size_t{1}, std::size_t{64}}) {
                SCOPED_TRACE(testing::Message() << "capacity " << capacity);
                FlatIdMap<std::uint64_t> map {capacity};
                constexpr std::uint64_t n {20'000};
                for (std::uint64_t i = 0; i < n; ++i) {
                    ASSERT_TRUE(map.insert(OrderId{i * 4096}, i)) << "insert #" << i;
                }
                EXPECT_EQ(map.size(), n);
                for (std::uint64_t i = 0; i < n; ++i) {
                    const std::uint64_t* value {map.find(OrderId{i * 4096})};
                    ASSERT_NE(value, nullptr) << "id " << i * 4096 << " lost while growing";
                    EXPECT_EQ(*value, i);
                    EXPECT_FALSE(map.contains(OrderId{i * 4096 + 1}));
                }
            }
        }

        TEST(FlatIdMap, MovedMapKeepsItsEntries) {
            FlatIdMap<std::uint64_t> original {4};
            for (std::uint64_t i = 0; i < 100; ++i) {
                ASSERT_TRUE(original.insert(OrderId{i}, i + 1000));
            }
            FlatIdMap<std::uint64_t> moved {std::move(original)};
            EXPECT_EQ(moved.size(), 100u);
            for (std::uint64_t i = 0; i < 100; ++i) {
                ASSERT_NE(moved.find(OrderId{i}), nullptr);
                EXPECT_EQ(*moved.find(OrderId{i}), i + 1000);
            }
            EXPECT_TRUE(moved.insert(OrderId{500}, 1));
            EXPECT_TRUE(moved.erase(OrderId{0}));
        }

        // ---- Collisions: probing, wrap-around and deletion inside a cluster ----

        // Checks that exactly the ids in `live` are present, each with value id + 1, and that a
        // few ids next to them are absent.
        template <class Map>
        void expect_exactly(Map& map, const std::vector<std::uint64_t>& live, std::uint64_t universe) {
            ASSERT_EQ(map.size(), live.size());
            std::vector<bool> is_live(universe, false);
            for (std::uint64_t id : live) {
                is_live[id] = true;
            }
            for (std::uint64_t id = 0; id < universe; ++id) {
                const auto* value {map.find(OrderId{id})};
                if (is_live[id]) {
                    ASSERT_NE(value, nullptr) << "id " << id << " should be present";
                    EXPECT_EQ(*value, id + 1) << "id " << id << " has the wrong value";
                } else {
                    EXPECT_EQ(value, nullptr) << "id " << id << " should be absent";
                    EXPECT_FALSE(map.contains(OrderId{id}));
                }
            }
        }

        // Erasing from one cluster: front, back, and middle. Each erase must leave every other key
        // in the cluster reachable. (With linear probing, simply emptying the erased bucket breaks
        // the probe chain for keys that were placed after it; backward-shift deletion moves them
        // back to close the gap.)
        template <class Hash>
        void erase_from_one_cluster() {
            FlatIdMap<std::uint64_t, Hash> map {64};
            constexpr std::uint64_t n {40};
            std::vector<std::uint64_t> live;
            for (std::uint64_t id = 0; id < n; ++id) {
                ASSERT_TRUE(map.insert(OrderId{id}, id + 1));
                live.push_back(id);
            }
            expect_exactly(map, live, n + 5);

            // Front of the cluster, back of it, then a run from the middle.
            for (std::uint64_t victim : {std::uint64_t{0}, n - 1, std::uint64_t{17}, std::uint64_t{18},
                                         std::uint64_t{19}, std::uint64_t{5}}) {
                SCOPED_TRACE(testing::Message() << "after erasing " << victim);
                ASSERT_TRUE(map.erase(OrderId{victim}));
                std::erase(live, victim);
                expect_exactly(map, live, n + 5);
            }

            // Refill the holes, then empty the map completely.
            for (std::uint64_t id : {std::uint64_t{0}, std::uint64_t{17}, n - 1}) {
                ASSERT_TRUE(map.insert(OrderId{id}, id + 1));
                live.push_back(id);
            }
            expect_exactly(map, live, n + 5);
            for (std::uint64_t id : live) {
                ASSERT_TRUE(map.erase(OrderId{id})) << "id " << id;
            }
            EXPECT_TRUE(map.empty());
            expect_exactly(map, {}, n + 5);
        }

        TEST(FlatIdMapCollisions, EraseInsideASingleCluster) {
            erase_from_one_cluster<CollideAll>();
        }

        TEST(FlatIdMapCollisions, EraseInsideAClusterThatWrapsAroundTheTable) {
            erase_from_one_cluster<WrapAround>();
        }

        TEST(FlatIdMapCollisions, EraseWhereClustersRunIntoEachOther) {
            erase_from_one_cluster<SevenBuckets>();
        }

        // Growth must re-place every key, including long collision chains.
        TEST(FlatIdMapCollisions, GrowingKeepsCollidingKeys) {
            FlatIdMap<std::uint64_t, CollideAll> map {1};
            std::vector<std::uint64_t> live;
            for (std::uint64_t id = 0; id < 300; ++id) {
                ASSERT_TRUE(map.insert(OrderId{id}, id + 1));
                live.push_back(id);
            }
            expect_exactly(map, live, 310);
        }

        // ---- Random churn against std::unordered_map ----
        //
        // Mixed insert / erase / find on a small id universe (so most operations hit live ids),
        // plus a few ids at the edges of the range. Every result is compared with
        // std::unordered_map, and every 500 operations every id is checked.
        template <class Hash>
        void churn_against_reference(std::uint64_t seed, int ops, std::size_t capacity) {
            FlatIdMap<std::uint64_t, Hash> map {capacity};
            std::unordered_map<std::uint64_t, std::uint64_t> reference;
            std::mt19937_64 rng {seed};

            const std::vector<std::uint64_t> edges {0, kMaxId, kMaxId - 1, std::uint64_t{1} << 63};
            constexpr std::uint64_t universe {600};
            auto pick_id = [&]() -> std::uint64_t {
                if (rng() % 100 == 0) {
                    return edges[rng() % edges.size()];
                }
                return rng() % universe;
            };

            for (int i = 0; i < ops; ++i) {
                const std::uint64_t id {pick_id()};
                const std::uint64_t roll {rng() % 10};
                if (roll < 5) {
                    const std::uint64_t value {rng()};
                    const bool expected {reference.emplace(id, value).second};
                    ASSERT_EQ(map.insert(OrderId{id}, value), expected) << "insert " << id << " at op " << i;
                } else if (roll < 8) {
                    const bool expected {reference.erase(id) == 1};
                    ASSERT_EQ(map.erase(OrderId{id}), expected) << "erase " << id << " at op " << i;
                } else {
                    const auto it {reference.find(id)};
                    const std::uint64_t* value {map.find(OrderId{id})};
                    if (it == reference.end()) {
                        ASSERT_EQ(value, nullptr) << "find " << id << " at op " << i;
                    } else {
                        ASSERT_NE(value, nullptr) << "find " << id << " at op " << i;
                        ASSERT_EQ(*value, it->second) << "find " << id << " at op " << i;
                    }
                }
                ASSERT_EQ(map.size(), reference.size()) << "at op " << i;

                if (i % 500 == 499) {
                    for (std::uint64_t probe = 0; probe < universe; ++probe) {
                        ASSERT_EQ(map.contains(OrderId{probe}), reference.contains(probe))
                            << "id " << probe << " after op " << i;
                    }
                    for (std::uint64_t probe : edges) {
                        ASSERT_EQ(map.contains(OrderId{probe}), reference.contains(probe))
                            << "id " << probe << " after op " << i;
                    }
                }
            }
        }

        TEST(FlatIdMapChurn, DefaultHashMatchesUnorderedMap) {
            for (std::uint64_t seed = 1; seed <= 3; ++seed) {
                SCOPED_TRACE(testing::Message() << "seed " << seed);
                churn_against_reference<OrderIdHash>(seed, 100'000, 16);
            }
        }

        TEST(FlatIdMapChurn, DefaultHashMatchesUnorderedMapFromCapacityZero) {
            churn_against_reference<OrderIdHash>(4, 50'000, 0);
        }

        TEST(FlatIdMapChurn, SevenBucketsMatchesUnorderedMap) {
            churn_against_reference<SevenBuckets>(5, 30'000, 16);
        }

        TEST(FlatIdMapChurn, WrapAroundMatchesUnorderedMap) {
            churn_against_reference<WrapAround>(6, 10'000, 16);
        }
    }
}
