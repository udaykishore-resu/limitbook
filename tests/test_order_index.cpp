#include <gtest/gtest.h>

#include <unordered_map>
#include <vector>

#include "limitbook/order_index.hpp"
#include "limitbook/sim/rng.hpp"

using limitbook::OrderId;
using limitbook::OrderIndex;

TEST(OrderIndex, InsertFindErase) {
    int a = 1, b = 2;
    OrderIndex<int> idx{16};
    EXPECT_TRUE(idx.insert(10, &a));
    EXPECT_TRUE(idx.insert(0, &b));  // key 0 is valid: emptiness is value==nullptr
    EXPECT_FALSE(idx.insert(10, &b));
    EXPECT_EQ(idx.find(10), &a);
    EXPECT_EQ(idx.find(0), &b);
    EXPECT_EQ(idx.find(11), nullptr);
    EXPECT_EQ(idx.erase(10), &a);
    EXPECT_EQ(idx.erase(10), nullptr);
    EXPECT_EQ(idx.find(10), nullptr);
    EXPECT_EQ(idx.size(), 1U);
}

TEST(OrderIndex, RefusesInsertPastMaxLoadInsteadOfRehashing) {
    OrderIndex<int> idx{8};
    int v = 0;
    std::size_t inserted = 0;
    for (OrderId k = 1; k < 10'000; ++k) {
        if (!idx.insert(k, &v)) {
            break;
        }
        ++inserted;
    }
    EXPECT_GE(inserted, 8U);
    EXPECT_LT(inserted, idx.capacity());
    EXPECT_EQ(idx.size(), inserted);
}

// Property test: random churn against std::unordered_map. Exercises
// backward-shift deletion across wrap-around and long probe runs.
TEST(OrderIndex, MatchesUnorderedMapUnderRandomChurn) {
    constexpr std::size_t kMax = 4096;
    OrderIndex<int> idx{kMax};
    std::unordered_map<OrderId, int*> ref;
    std::vector<int> storage(64);
    limitbook::sim::Rng rng{7};
    std::vector<OrderId> keys;

    for (int step = 0; step < 200'000; ++step) {
        const auto r = rng.below(100);
        if (r < 55 && ref.size() < kMax) {
            const OrderId k = rng.below(20'000);
            int* v = &storage[rng.below(storage.size())];
            const bool fresh = !ref.contains(k);
            EXPECT_EQ(idx.insert(k, v), fresh);
            if (fresh) {
                ref.emplace(k, v);
                keys.push_back(k);
            }
        } else if (!keys.empty()) {
            const std::size_t pos = rng.below(keys.size());
            const OrderId k = keys[pos];
            keys[pos] = keys.back();
            keys.pop_back();
            ASSERT_EQ(idx.erase(k), ref.at(k));
            ref.erase(k);
        }
        if (step % 997 == 0) {
            for (const auto& [k, v] : ref) {
                ASSERT_EQ(idx.find(k), v) << "key " << k << " at step " << step;
            }
        }
    }
    EXPECT_EQ(idx.size(), ref.size());
}
