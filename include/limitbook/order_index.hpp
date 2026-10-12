#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "limitbook/types.hpp"

namespace limitbook {

// Open-addressing hash map OrderId -> V*, sized once at construction.
//
// * Linear probing over a flat array: one cache miss per lookup in the
//   common case, versus a pointer chase per bucket in std::unordered_map.
// * Backward-shift deletion: no tombstones, so probe lengths do not decay
//   over a trading day of add/delete churn.
// * Never rehashes. insert() returns false at 7/8 load instead of
//   allocating on the hot path; size it for the peak live-order count.
template <typename V>
class OrderIndex {
    struct Slot {
        OrderId key;
        V* value;  // nullptr marks an empty slot, so any key (even 0) is valid.
    };

public:
    explicit OrderIndex(std::size_t max_entries)
        : capacity_(std::bit_ceil(max_entries + max_entries / 7 + 16)),
          mask_(capacity_ - 1),
          max_load_(capacity_ - capacity_ / 8),
          slots_(std::make_unique<Slot[]>(capacity_)) {}

    OrderIndex(const OrderIndex&) = delete;
    OrderIndex& operator=(const OrderIndex&) = delete;
    OrderIndex(OrderIndex&&) = delete;
    OrderIndex& operator=(OrderIndex&&) = delete;
    ~OrderIndex() = default;

    [[nodiscard]] bool insert(OrderId key, V* value) noexcept {
        if (size_ >= max_load_) [[unlikely]] {
            return false;
        }
        std::size_t i = home(key);
        while (slots_[i].value != nullptr) {
            if (slots_[i].key == key) {
                return false;  // duplicate
            }
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{key, value};
        ++size_;
        return true;
    }

    [[nodiscard]] V* find(OrderId key) const noexcept {
        std::size_t i = home(key);
        while (slots_[i].value != nullptr) {
            if (slots_[i].key == key) {
                return slots_[i].value;
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    // Returns the removed value, or nullptr if the key was absent.
    V* erase(OrderId key) noexcept {
        std::size_t i = home(key);
        while (true) {
            if (slots_[i].value == nullptr) {
                return nullptr;
            }
            if (slots_[i].key == key) {
                break;
            }
            i = (i + 1) & mask_;
        }
        V* removed = slots_[i].value;

        // Backward-shift: pull later entries of the same probe run into the
        // hole unless doing so would move one before its home slot.
        std::size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].value == nullptr) {
                break;
            }
            const std::size_t k = home(slots_[j].key);
            const bool home_in_gap = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (!home_in_gap) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].value = nullptr;
        --size_;
        return removed;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    // murmur3 fmix64: ITCH order refs are sequential, so an identity hash would
    // cluster them into one long probe run.
    [[nodiscard]] static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }
    [[nodiscard]] std::size_t home(OrderId key) const noexcept {
        return static_cast<std::size_t>(mix(key)) & mask_;
    }

    std::size_t capacity_;
    std::size_t mask_;
    std::size_t max_load_;
    std::size_t size_{0};
    std::unique_ptr<Slot[]> slots_;
};

}  // namespace limitbook
