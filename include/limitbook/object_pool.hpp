#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace limitbook {

// Fixed-capacity object pool with an intrusive free list.
//
// * Exactly one heap allocation, in the constructor. create()/destroy() never
//   touch the global allocator, so the hot path cannot stall in malloc.
// * Exhaustion is reported by returning nullptr, not by throwing: the hot path
//   is compiled with -fno-exceptions and the caller decides the policy.
// * LIFO reuse: the most recently freed slot is handed out next, which keeps
//   the working set warm in L1/L2.
template <typename T>
class ObjectPool {
    union Slot {
        Slot* next;
        alignas(T) std::byte storage[sizeof(T)];
    };
    static_assert(std::is_trivially_destructible_v<Slot>);

public:
    explicit ObjectPool(std::size_t capacity)
        : slots_(capacity == 0 ? nullptr : std::make_unique<Slot[]>(capacity)),
          capacity_(capacity) {
        for (std::size_t i = 0; i + 1 < capacity_; ++i) {
            slots_[i].next = &slots_[i + 1];
        }
        if (capacity_ > 0) {
            slots_[capacity_ - 1].next = nullptr;
            free_ = &slots_[0];
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;
    ~ObjectPool() = default;  // Owners must destroy live objects first.

    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        if (free_ == nullptr) [[unlikely]] {
            return nullptr;
        }
        Slot* slot = free_;
        free_ = slot->next;
        ++live_;
        return std::construct_at(reinterpret_cast<T*>(slot->storage), std::forward<Args>(args)...);
    }

    void destroy(T* obj) noexcept {
        std::destroy_at(obj);
        // The object lived at the start of the slot, so the addresses coincide.
        auto* slot = reinterpret_cast<Slot*>(obj);
        slot->next = free_;
        free_ = slot;
        --live_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t available() const noexcept { return capacity_ - live_; }

private:
    std::unique_ptr<Slot[]> slots_;
    Slot* free_{nullptr};
    std::size_t capacity_{0};
    std::size_t live_{0};
};

}  // namespace limitbook
