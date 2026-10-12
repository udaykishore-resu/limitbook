#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "limitbook/object_pool.hpp"

using limitbook::ObjectPool;

TEST(ObjectPool, HandsOutDistinctSlotsUntilExhausted) {
    ObjectPool<int> pool{4};
    std::set<int*> seen;
    for (int i = 0; i < 4; ++i) {
        int* p = pool.create(i);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(*p, i);
        EXPECT_TRUE(seen.insert(p).second);
    }
    EXPECT_EQ(pool.create(99), nullptr);
    EXPECT_EQ(pool.live(), 4U);
    EXPECT_EQ(pool.available(), 0U);
}

TEST(ObjectPool, ReusesMostRecentlyFreedSlot) {
    ObjectPool<int> pool{8};
    int* a = pool.create(1);
    int* b = pool.create(2);
    pool.destroy(a);
    EXPECT_EQ(pool.create(3), a);  // LIFO: hot slot comes back first
    pool.destroy(b);
    EXPECT_EQ(pool.live(), 1U);
}

TEST(ObjectPool, RunsConstructorsAndDestructors) {
    ObjectPool<std::string> pool{2};
    std::string* s = pool.create("a string long enough to defeat small-string optimisation");
    ASSERT_NE(s, nullptr);
    EXPECT_GT(s->size(), 40U);
    pool.destroy(s);  // ASan reports a leak if the destructor did not run
    EXPECT_EQ(pool.live(), 0U);
}

TEST(ObjectPool, RespectsOverAlignedTypes) {
    struct alignas(64) Wide {
        double x;
    };
    ObjectPool<Wide> pool{3};
    for (int i = 0; i < 3; ++i) {
        Wide* w = pool.create();
        ASSERT_NE(w, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(w) % 64, 0U);
    }
}

TEST(ObjectPool, ZeroCapacityIsAlwaysExhausted) {
    ObjectPool<int> pool{0};
    EXPECT_EQ(pool.create(1), nullptr);
}
