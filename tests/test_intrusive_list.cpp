#include <gtest/gtest.h>

#include <vector>

#include "limitbook/intrusive_list.hpp"

namespace {

struct Node : limitbook::ListHook {
    explicit Node(int v) : value(v) {}
    int value;
};

std::vector<int> values(const limitbook::IntrusiveList<Node>& list) {
    std::vector<int> out;
    for (const Node& n : list) {
        out.push_back(n.value);
    }
    return out;
}

}  // namespace

TEST(IntrusiveList, PushBackPreservesFifoOrder) {
    Node a{1}, b{2}, c{3};
    limitbook::IntrusiveList<Node> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    EXPECT_EQ(values(list), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(list.front(), &a);
    EXPECT_EQ(list.back(), &c);
    EXPECT_EQ(list.size(), 3U);
}

TEST(IntrusiveList, EraseHeadMiddleTail) {
    Node a{1}, b{2}, c{3}, d{4};
    limitbook::IntrusiveList<Node> list;
    for (Node* n : {&a, &b, &c, &d}) {
        list.push_back(n);
    }
    list.erase(&b);
    EXPECT_EQ(values(list), (std::vector<int>{1, 3, 4}));
    list.erase(&a);
    EXPECT_EQ(values(list), (std::vector<int>{3, 4}));
    list.erase(&d);
    EXPECT_EQ(values(list), (std::vector<int>{3}));
    list.erase(&c);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.front(), nullptr);
    EXPECT_EQ(list.back(), nullptr);
}

TEST(IntrusiveList, ErasedNodeCanBeRelinked) {
    Node a{1}, b{2};
    limitbook::IntrusiveList<Node> list;
    list.push_back(&a);
    list.push_back(&b);
    list.erase(&a);
    list.push_back(&a);  // loses priority: goes to the back
    EXPECT_EQ(values(list), (std::vector<int>{2, 1}));
}
