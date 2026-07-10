#include "engine.hpp"

namespace me {

// Defined in src/naive/order_book.cpp.
IOrderBook* create_naive_order_book();

// Defined in src/optimized/order_book.cpp. V2 and V3 share the exact same
// data structures and matching logic (array-indexed price levels + intrusive
// list) -- the only difference between them is which allocator the *caller*
// uses to create Order objects before handing them to the book (heap new/delete
// for V2, Arena for V3). The book itself never allocates or frees Order memory
// (matching V1's behavior: ownership transfer described in order_book.hpp is
// about the book's internal FIFO containers, not literal free() responsibility),
// so one class correctly serves both BookKind values.
IOrderBook* create_optimized_order_book(int64_t min_price, int64_t tick);

std::unique_ptr<IOrderBook> make_order_book(BookKind kind, int64_t min_price, int64_t tick) {
    switch (kind) {
        case BookKind::Naive:
            return std::unique_ptr<IOrderBook>(create_naive_order_book());
        case BookKind::Optimized:
        case BookKind::OptimizedArena:
            return std::unique_ptr<IOrderBook>(create_optimized_order_book(min_price, tick));
    }
    return nullptr;  // unreachable; silences -Wreturn-type on some compilers
}

}  // namespace me
