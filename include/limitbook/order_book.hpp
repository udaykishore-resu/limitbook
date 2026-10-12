#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "limitbook/intrusive_list.hpp"
#include "limitbook/object_pool.hpp"
#include "limitbook/types.hpp"

namespace limitbook {

struct PriceLevel;

// 48 bytes on LP64. Orders at a price are linked through the embedded hook,
// so time priority is simply list order and needs no timestamp comparison.
struct Order : ListHook {
    OrderId id{0};
    Price price{0};
    Qty qty{0};
    Side side{Side::Buy};
    PriceLevel* level{nullptr};
};

struct PriceLevel {
    explicit PriceLevel(Price p) noexcept : price(p) {}

    Price price;
    std::uint64_t total_qty{0};
    IntrusiveList<Order> orders;

    [[nodiscard]] std::uint32_t order_count() const noexcept {
        return static_cast<std::uint32_t>(orders.size());
    }
};

// All memory a set of books will ever touch, allocated up front.
struct BookArena {
    BookArena(std::size_t max_orders, std::size_t max_levels) : orders(max_orders), levels(max_levels) {}

    ObjectPool<Order> orders;
    ObjectPool<PriceLevel> levels;
};

struct Quote {
    Price price;
    std::uint64_t qty;
    std::uint32_t orders;

    bool operator==(const Quote&) const = default;
};

// One side of the book: a contiguous vector of level pointers sorted from
// worst to best, so the best level is levels_.back().
//
// Why not std::map? Real order flow concentrates within a few ticks of the
// touch. Scanning backwards from the best level finds or inserts those prices
// in a handful of comparisons over contiguous memory, and erasing near the
// top shifts only the few better levels. std::map pays a pointer chase per
// tree level and an allocation per new price.
template <Side S>
class BookSide {
public:
    explicit BookSide(std::size_t level_reserve) { levels_.reserve(level_reserve); }

    // True if price a has higher priority than price b on this side.
    [[nodiscard]] static constexpr bool better(Price a, Price b) noexcept {
        if constexpr (S == Side::Buy) {
            return a > b;
        } else {
            return a < b;
        }
    }

    [[nodiscard]] PriceLevel* best() const noexcept { return levels_.empty() ? nullptr : levels_.back(); }
    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }
    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

    [[nodiscard]] PriceLevel* find(Price price) const noexcept {
        for (std::size_t i = levels_.size(); i > 0; --i) {
            PriceLevel* lvl = levels_[i - 1];
            if (lvl->price == price) {
                return lvl;
            }
            if (!better(lvl->price, price)) {
                return nullptr;
            }
        }
        return nullptr;
    }

    // Returns the level for `price`, creating it if needed. nullptr only if
    // the level pool is exhausted. May grow the vector past its reservation,
    // which is the one allocation this structure can make after warmup.
    [[nodiscard]] PriceLevel* find_or_insert(Price price, ObjectPool<PriceLevel>& pool) noexcept {
        std::size_t i = levels_.size();
        while (i > 0 && better(levels_[i - 1]->price, price)) {
            --i;
        }
        if (i > 0 && levels_[i - 1]->price == price) {
            return levels_[i - 1];
        }
        PriceLevel* lvl = pool.create(price);
        if (lvl == nullptr) [[unlikely]] {
            return nullptr;
        }
        levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), lvl);
        return lvl;
    }

    void erase(const PriceLevel* lvl) noexcept {
        for (std::size_t i = levels_.size(); i > 0; --i) {
            if (levels_[i - 1] == lvl) {
                levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i - 1));
                return;
            }
        }
    }

    // Visit up to `max_levels` levels from best to worst.
    template <typename F>
    void for_each_level(std::size_t max_levels, F&& visit) const {
        std::size_t visited = 0;
        for (std::size_t i = levels_.size(); i > 0 && visited < max_levels; --i, ++visited) {
            visit(static_cast<const PriceLevel&>(*levels_[i - 1]));
        }
    }

    [[nodiscard]] std::vector<PriceLevel*>& raw() noexcept { return levels_; }

private:
    std::vector<PriceLevel*> levels_;
};

// Full-depth (L3) limit order book for one instrument. It owns no memory of
// its own beyond the two level vectors; orders and levels come from a shared
// BookArena, so thousands of books can share one pre-sized pool.
//
// Order-id lookup is deliberately NOT part of the book: ITCH order refs are
// unique across all instruments, so the index lives one layer up (BookBuilder
// or MatchingEngine) and the book only manipulates Order pointers.
class OrderBook {
public:
    explicit OrderBook(BookArena& arena, std::size_t level_reserve = 256)
        : arena_(&arena), bids_(level_reserve), asks_(level_reserve) {}

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;
    ~OrderBook() { clear(); }

    // nullptr if the arena is exhausted (book unchanged).
    [[nodiscard]] Order* add(OrderId id, Side side, Price price, Qty qty) noexcept {
        Order* order = arena_->orders.create();
        if (order == nullptr) [[unlikely]] {
            return nullptr;
        }
        PriceLevel* lvl = side == Side::Buy ? bids_.find_or_insert(price, arena_->levels)
                                            : asks_.find_or_insert(price, arena_->levels);
        if (lvl == nullptr) [[unlikely]] {
            arena_->orders.destroy(order);
            return nullptr;
        }
        order->id = id;
        order->price = price;
        order->qty = qty;
        order->side = side;
        order->level = lvl;
        lvl->orders.push_back(order);
        lvl->total_qty += qty;
        return order;
    }

    // Partial cancel or execution. Returns true if the order was fully
    // consumed and destroyed; the pointer is dangling afterwards.
    bool reduce(Order* order, Qty by) noexcept {
        if (by >= order->qty) {
            remove(order);
            return true;
        }
        order->qty -= by;
        order->level->total_qty -= by;
        return false;
    }

    void remove(Order* order) noexcept {
        PriceLevel* lvl = order->level;
        lvl->orders.erase(order);
        lvl->total_qty -= order->qty;
        if (lvl->orders.empty()) {
            if (order->side == Side::Buy) {
                bids_.erase(lvl);
            } else {
                asks_.erase(lvl);
            }
            arena_->levels.destroy(lvl);
        }
        arena_->orders.destroy(order);
    }

    [[nodiscard]] PriceLevel* best_level(Side side) const noexcept {
        return side == Side::Buy ? bids_.best() : asks_.best();
    }

    [[nodiscard]] std::optional<Quote> best_bid() const noexcept { return quote(bids_.best()); }
    [[nodiscard]] std::optional<Quote> best_ask() const noexcept { return quote(asks_.best()); }

    [[nodiscard]] bool crossed() const noexcept {
        const PriceLevel* bid = bids_.best();
        const PriceLevel* ask = asks_.best();
        return bid != nullptr && ask != nullptr && bid->price >= ask->price;
    }

    [[nodiscard]] std::size_t level_count(Side side) const noexcept {
        return side == Side::Buy ? bids_.size() : asks_.size();
    }

    template <typename F>
    void for_each_level(Side side, std::size_t max_levels, F&& visit) const {
        if (side == Side::Buy) {
            bids_.for_each_level(max_levels, visit);
        } else {
            asks_.for_each_level(max_levels, visit);
        }
    }

    // Return every order and level to the arena.
    void clear() noexcept {
        clear_side(bids_.raw());
        clear_side(asks_.raw());
    }

private:
    [[nodiscard]] static std::optional<Quote> quote(const PriceLevel* lvl) noexcept {
        if (lvl == nullptr) {
            return std::nullopt;
        }
        return Quote{lvl->price, lvl->total_qty, lvl->order_count()};
    }

    void clear_side(std::vector<PriceLevel*>& levels) noexcept {
        for (PriceLevel* lvl : levels) {
            Order* order = lvl->orders.front();
            while (order != nullptr) {
                Order* next = IntrusiveList<Order>::next(order);
                lvl->orders.erase(order);
                arena_->orders.destroy(order);
                order = next;
            }
            arena_->levels.destroy(lvl);
        }
        levels.clear();
    }

    BookArena* arena_;
    BookSide<Side::Buy> bids_;
    BookSide<Side::Sell> asks_;
};

}  // namespace limitbook
