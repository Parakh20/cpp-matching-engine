#ifndef MATCHING_ENGINE_ARENA_HPP
#define MATCHING_ENGINE_ARENA_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "order.hpp"

namespace me {

// Bump-pointer arena allocator specialized for `Order` objects (V3).
//
// Rationale: in the naive engine every resting order triggers a heap allocation
// (std::queue node) and every cancel a free. malloc/free in the hot path costs
// hundreds of nanoseconds and, worse, has a long tail (lock contention, page
// faults, free-list walks) that blows up p99 latency. The arena replaces that
// with:
//   - allocation = pop a recycled Order off a free list, else bump a pointer
//     within a pre-reserved 1 MiB block (amortized O(1), no syscall);
//   - free = push the Order back onto the free list (O(1), no syscall).
// In steady state (alloc/free balanced, as in a live book) there are zero calls
// to the system allocator, giving flat, predictable latency.
//
// Single-threaded by design (one matching thread per book). A lock-free MPSC
// free list is an optional extension noted in the README.
class Arena {
public:
    static constexpr size_t BLOCK_SIZE = 1u << 20;  // 1 MiB raw bytes per block
    static constexpr size_t ORDERS_PER_BLOCK = BLOCK_SIZE / sizeof(Order);

    Arena() = default;

    // Non-copyable, non-movable: it hands out stable pointers into owned blocks.
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    ~Arena() {
        for (Order* block : blocks_) {
            ::operator delete[](static_cast<void*>(block));
        }
    }

    // Allocate one Order. Reuses a freed slot when available, otherwise bumps
    // within the current block (allocating a fresh block only when exhausted).
    // The returned Order is default-constructed in place.
    Order* alloc_order() {
        if (free_head_ != nullptr) {
            Order* o = free_head_;
            free_head_ = free_head_->next;  // free list threads through `next`
            *o = Order{};                   // reset to a clean state
            ++live_count_;
            return o;
        }
        if (bump_index_ >= ORDERS_PER_BLOCK || blocks_.empty()) {
            add_block();
        }
        Order* o = &blocks_.back()[bump_index_++];
        *o = Order{};
        ++live_count_;
        return o;
    }

    // Return an Order to the free list for reuse. The pointer must have come
    // from this arena. O(1), never touches the system allocator.
    void free_order(Order* o) {
        if (o == nullptr) return;
        o->next = free_head_;
        free_head_ = o;
        --live_count_;
    }

    // Diagnostics (used by tests / README claims).
    size_t blocks_allocated() const { return blocks_.size(); }
    size_t live_count() const { return live_count_; }

private:
    void add_block() {
        // Raw, uninitialized storage; alloc_order() placement-resets each slot.
        Order* block = static_cast<Order*>(::operator new[](BLOCK_SIZE));
        blocks_.push_back(block);
        bump_index_ = 0;
    }

    std::vector<Order*> blocks_;
    size_t bump_index_ = ORDERS_PER_BLOCK;  // force a block on first alloc
    Order* free_head_ = nullptr;
    size_t live_count_ = 0;
};

}  // namespace me

#endif  // MATCHING_ENGINE_ARENA_HPP
