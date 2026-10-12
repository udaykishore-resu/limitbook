#pragma once

// gtest falls back to a hex dump (padding bytes included) for types without
// a printer; these make differential-test failures readable.

#include <ostream>

#include "limitbook/matching_engine.hpp"
#include "limitbook/order_book.hpp"

namespace limitbook {

inline void PrintTo(const Quote& q, std::ostream* os) {
    *os << "{px=" << q.price << " qty=" << q.qty << " n=" << q.orders << "}";
}

inline void PrintTo(const Fill& f, std::ostream* os) {
    *os << "{taker=" << f.taker << " maker=" << f.maker << " px=" << f.price << " qty=" << f.qty << "}";
}

}  // namespace limitbook
