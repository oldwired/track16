// A bounded lock-free queue for events that cross threads (reader threads and the UI push, the audio thread pops).
#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace app
{
// Bounded lock-free queue, many producers, one consumer (D. Vyukov's MPMC design): reader threads and the UI push,
// the audio thread pops. Never blocks or allocates; push() returns false when full. T must be trivially copyable.
template <typename T, std::size_t Capacity>
class MpscQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity >= 2, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "items cross threads by copy");

public:
    MpscQueue() noexcept
    {
        for (std::size_t i = 0; i < Capacity; ++i)
            cells_[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool push(const T& item) noexcept
    {
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;)
        {
            Cell& cell = cells_[pos & (Capacity - 1)];
            const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0)
            {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.item = item;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
                return false; // full
            else
                pos = tail_.load(std::memory_order_relaxed);
        }
    }

    bool pop(T& out) noexcept // the single consumer
    {
        Cell& cell = cells_[head_ & (Capacity - 1)];
        const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
        if (static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(head_ + 1) < 0)
            return false; // empty
        out = cell.item;
        cell.sequence.store(head_ + Capacity, std::memory_order_release);
        ++head_;
        return true;
    }

private:
    struct Cell
    {
        std::atomic<std::size_t> sequence;
        T item;
    };
    Cell cells_[Capacity];
    alignas(64) std::atomic<std::size_t> tail_ {0};
    alignas(64) std::size_t head_ = 0;
};
} // namespace app
