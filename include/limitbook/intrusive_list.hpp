#pragma once

#include <concepts>
#include <cstddef>
#include <iterator>

namespace limitbook {

// Hook embedded in every element. The list never allocates: linking an
// element is two pointer writes, and erase is O(1) given the element.
// std::list would allocate a node per order and scatter them across the heap.
struct ListHook {
    ListHook* prev{nullptr};
    ListHook* next{nullptr};
};

template <typename T>
    requires std::derived_from<T, ListHook>
class IntrusiveList {
public:
    class iterator {
    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = T&;
        using pointer = T*;
        using iterator_category = std::forward_iterator_tag;

        iterator() = default;
        explicit iterator(ListHook* node) noexcept : node_(node) {}

        reference operator*() const noexcept { return *static_cast<T*>(node_); }
        pointer operator->() const noexcept { return static_cast<T*>(node_); }
        iterator& operator++() noexcept {
            node_ = node_->next;
            return *this;
        }
        iterator operator++(int) noexcept {
            iterator tmp = *this;
            ++*this;
            return tmp;
        }
        bool operator==(const iterator&) const = default;

    private:
        ListHook* node_{nullptr};
    };

    IntrusiveList() = default;
    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;
    IntrusiveList(IntrusiveList&&) = delete;
    IntrusiveList& operator=(IntrusiveList&&) = delete;
    ~IntrusiveList() = default;

    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] T* front() const noexcept { return static_cast<T*>(head_); }
    [[nodiscard]] T* back() const noexcept { return static_cast<T*>(tail_); }

    void push_back(T* element) noexcept {
        ListHook* node = element;
        node->prev = tail_;
        node->next = nullptr;
        if (tail_ != nullptr) {
            tail_->next = node;
        } else {
            head_ = node;
        }
        tail_ = node;
        ++size_;
    }

    void erase(T* element) noexcept {
        ListHook* node = element;
        if (node->prev != nullptr) {
            node->prev->next = node->next;
        } else {
            head_ = node->next;
        }
        if (node->next != nullptr) {
            node->next->prev = node->prev;
        } else {
            tail_ = node->prev;
        }
        node->prev = nullptr;
        node->next = nullptr;
        --size_;
    }

    [[nodiscard]] static T* next(T* element) noexcept {
        return static_cast<T*>(static_cast<ListHook*>(element)->next);
    }

    [[nodiscard]] iterator begin() const noexcept { return iterator{head_}; }
    [[nodiscard]] iterator end() const noexcept { return iterator{}; }

private:
    ListHook* head_{nullptr};
    ListHook* tail_{nullptr};
    std::size_t size_{0};
};

}  // namespace limitbook
