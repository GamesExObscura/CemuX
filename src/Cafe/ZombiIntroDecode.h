#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace ZombiIntroDecode
{
inline std::atomic_uint32_t startupSwapsRemaining{0};
inline void BeginStartup() { startupSwapsRemaining.store(32, std::memory_order_relaxed); }
inline void EndStartup() { startupSwapsRemaining.store(0, std::memory_order_relaxed); }
inline bool IsStartupPending() { return startupSwapsRemaining.load(std::memory_order_relaxed) != 0; }
inline bool TakeStartupSwap()
{
    // A skipped/aborted logo must never leave gameplay in startup submission mode.
    auto remaining = startupSwapsRemaining.load(std::memory_order_relaxed);
    while (remaining && !startupSwapsRemaining.compare_exchange_weak(
        remaining, remaining - 1, std::memory_order_relaxed)) {}
    return remaining != 0;
}

constexpr bool IsTarget(std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
{
    return title == 0x000500001010dd00ull && version == 32 && rpx == 0x577202f4;
}

constexpr bool IsIntro(std::uint32_t width, std::uint32_t height, std::uint32_t frames,
    std::uint32_t rate, std::uint32_t divisor)
{
    return width == 1280 && height == 720 && frames == 241 &&
        rate == 10000000 && divisor == 333333;
}

constexpr std::array<std::array<std::uint32_t, 2>, 3> Calls{{
    {0x027fd00c, 0x4800ad4d},
    {0x027fd108, 0x4800ac51},
    {0x027fd3e0, 0x4800a979},
}};
constexpr std::uint32_t Async = 0x02807d58;
constexpr std::uint32_t Wait = 0x028079a8;
}
