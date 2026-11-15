#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace hype
{
/**
    Wait-free single-producer / single-consumer ring buffer.

    Used for every hand-off between the audio thread and the rest of the
    application. Neither push() nor pop() allocates, locks or blocks, so both are
    safe to call from the real-time audio callback.

    If more than one thread needs to produce, the producers must serialise among
    themselves (the Deck does this with a SpinLock that the audio thread never touches).
*/
template <typename T, size_t Capacity>
class SpscQueue
{
    static_assert ((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert (std::is_trivially_copyable_v<T>, "SpscQueue only holds trivially copyable items");

public:
    bool push (const T& item) noexcept
    {
        const auto w = writeIndex.load (std::memory_order_relaxed);
        const auto r = readIndex.load (std::memory_order_acquire);

        if (w - r >= Capacity)
            return false;

        items[w & mask] = item;
        writeIndex.store (w + 1, std::memory_order_release);
        return true;
    }

    bool pop (T& item) noexcept
    {
        const auto r = readIndex.load (std::memory_order_relaxed);
        const auto w = writeIndex.load (std::memory_order_acquire);

        if (r == w)
            return false;

        item = items[r & mask];
        readIndex.store (r + 1, std::memory_order_release);
        return true;
    }

    size_t sizeApprox() const noexcept
    {
        return writeIndex.load (std::memory_order_acquire) - readIndex.load (std::memory_order_acquire);
    }

private:
    static constexpr size_t mask = Capacity - 1;
    std::array<T, Capacity> items {};
    alignas (64) std::atomic<size_t> writeIndex { 0 };
    alignas (64) std::atomic<size_t> readIndex { 0 };
};
} // namespace hype
