#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "limitbook/itch/messages.hpp"
#include "limitbook/order_book.hpp"
#include "limitbook/order_index.hpp"
#include "limitbook/types.hpp"

namespace limitbook::itch {

// Rebuilds a full-depth book for every instrument from an ITCH 5.0 stream.
// Pass it to itch::decode / itch::for_each_framed as the handler.
//
// Memory model: one BookArena (orders + levels) and one OrderIndex shared by
// all instruments, sized from Config at construction. Books are created when
// their Stock Directory message arrives (pre-market), so no allocation
// happens during continuous trading unless a book outgrows level_reserve.
class BookBuilder {
public:
    static constexpr std::size_t kMaxLocates = 65536;

    struct Config {
        std::size_t max_orders = std::size_t{1} << 22;  // peak live orders, all symbols
        std::size_t max_levels = std::size_t{1} << 20;  // peak live price levels, all symbols
        std::size_t level_reserve = 64;                 // per side, per book
        bool validate = false;                          // run invariant checks per message
    };

    struct Stats {
        std::uint64_t adds{0};
        std::uint64_t executions{0};
        std::uint64_t cancels{0};
        std::uint64_t deletes{0};
        std::uint64_t replaces{0};
        std::uint64_t unknown_refs{0};       // exec/cancel/delete/replace for an order we never saw
        std::uint64_t capacity_failures{0};  // pool or index exhausted: raise Config limits
        // Validation (Config::validate). Non-zero values are expected around
        // the opening/closing crosses; outside them they indicate a bug.
        std::uint64_t executions_off_top{0};  // 'E' against an order not at the best price
        std::uint64_t crossed_after_add{0};   // best bid >= best ask after an add
    };

    explicit BookBuilder(const Config& cfg)
        : cfg_(cfg), arena_(cfg.max_orders, cfg.max_levels), index_(cfg.max_orders), books_(kMaxLocates) {
        symbols_.resize(kMaxLocates);
    }

    BookBuilder(const BookBuilder&) = delete;
    BookBuilder& operator=(const BookBuilder&) = delete;
    BookBuilder(BookBuilder&&) = delete;
    BookBuilder& operator=(BookBuilder&&) = delete;
    ~BookBuilder() {
        books_.clear();  // books return their orders to the arena before it dies
    }

    void operator()(const StockDirectory& m) {
        auto& sym = symbols_[m.stock_locate()];
        const std::string_view s = m.stock();
        sym.fill(' ');
        for (std::size_t i = 0; i < s.size() && i < sym.size(); ++i) {
            sym[i] = s[i];
        }
        book_for(m.stock_locate());
    }

    void operator()(const AddOrder& m) noexcept {
        add(m.stock_locate(), m.order_ref(), m.side(), static_cast<Price>(m.price()), m.shares());
    }

    void operator()(const AddOrderMpid& m) noexcept {
        add(m.stock_locate(), m.order_ref(), m.side(), static_cast<Price>(m.price()), m.shares());
    }

    void operator()(const OrderExecuted& m) noexcept {
        ++stats_.executions;
        Order* order = index_.find(m.order_ref());
        if (order == nullptr) [[unlikely]] {
            ++stats_.unknown_refs;
            return;
        }
        OrderBook& book = book_for(m.stock_locate());
        if (cfg_.validate) {
            const PriceLevel* best = book.best_level(order->side);
            if (best == nullptr || best->price != order->price) {
                ++stats_.executions_off_top;
            }
        }
        reduce(book, order, m.order_ref(), m.executed_shares());
    }

    void operator()(const OrderExecutedWithPrice& m) noexcept {
        ++stats_.executions;
        Order* order = index_.find(m.order_ref());
        if (order == nullptr) [[unlikely]] {
            ++stats_.unknown_refs;
            return;
        }
        reduce(book_for(m.stock_locate()), order, m.order_ref(), m.executed_shares());
    }

    void operator()(const OrderCancel& m) noexcept {
        ++stats_.cancels;
        Order* order = index_.find(m.order_ref());
        if (order == nullptr) [[unlikely]] {
            ++stats_.unknown_refs;
            return;
        }
        reduce(book_for(m.stock_locate()), order, m.order_ref(), m.cancelled_shares());
    }

    void operator()(const OrderDelete& m) noexcept {
        ++stats_.deletes;
        Order* order = index_.erase(m.order_ref());
        if (order == nullptr) [[unlikely]] {
            ++stats_.unknown_refs;
            return;
        }
        book_for(m.stock_locate()).remove(order);
    }

    // Replace = delete original + add new ref at new price/size on the same
    // side. The new order goes to the back of its level: priority is lost.
    void operator()(const OrderReplace& m) noexcept {
        ++stats_.replaces;
        Order* order = index_.erase(m.original_order_ref());
        if (order == nullptr) [[unlikely]] {
            ++stats_.unknown_refs;
            return;
        }
        const Side side = order->side;
        OrderBook& book = book_for(m.stock_locate());
        book.remove(order);
        insert(book, m.new_order_ref(), side, static_cast<Price>(m.price()), m.shares());
    }

    [[nodiscard]] const OrderBook* book(std::uint16_t locate) const noexcept { return books_[locate].get(); }

    [[nodiscard]] std::string_view symbol(std::uint16_t locate) const noexcept {
        const auto& sym = symbols_[locate];
        std::size_t len = sym.size();
        while (len > 0 && (sym[len - 1] == ' ' || sym[len - 1] == '\0')) {
            --len;
        }
        return {sym.data(), len};
    }

    [[nodiscard]] std::optional<std::uint16_t> locate_of(std::string_view sym) const noexcept {
        for (std::size_t i = 0; i < kMaxLocates; ++i) {
            if (books_[i] && symbol(static_cast<std::uint16_t>(i)) == sym) {
                return static_cast<std::uint16_t>(i);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t book_count() const noexcept {
        std::size_t n = 0;
        for (const auto& b : books_) {
            if (b) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t live_orders() const noexcept { return index_.size(); }
    [[nodiscard]] const BookArena& arena() const noexcept { return arena_; }

private:
    OrderBook& book_for(std::uint16_t locate) {
        auto& slot = books_[locate];
        if (!slot) [[unlikely]] {
            slot = std::make_unique<OrderBook>(arena_, cfg_.level_reserve);
        }
        return *slot;
    }

    void add(std::uint16_t locate, OrderId ref, Side side, Price price, Qty shares) noexcept {
        ++stats_.adds;
        OrderBook& book = book_for(locate);
        insert(book, ref, side, price, shares);
        if (cfg_.validate && book.crossed()) {
            ++stats_.crossed_after_add;
        }
    }

    void insert(OrderBook& book, OrderId ref, Side side, Price price, Qty shares) noexcept {
        Order* order = book.add(ref, side, price, shares);
        if (order == nullptr) [[unlikely]] {
            ++stats_.capacity_failures;
            return;
        }
        if (!index_.insert(ref, order)) [[unlikely]] {
            book.remove(order);
            ++stats_.capacity_failures;
        }
    }

    void reduce(OrderBook& book, Order* order, OrderId ref, Qty by) noexcept {
        if (by >= order->qty) {
            index_.erase(ref);
        }
        book.reduce(order, by);
    }

    Config cfg_;
    BookArena arena_;
    OrderIndex<Order> index_;
    std::vector<std::unique_ptr<OrderBook>> books_;
    std::vector<std::array<char, 8>> symbols_;
    Stats stats_;
};

}  // namespace limitbook::itch
