#pragma once
#include <cstdint>

namespace ZombiSwapSubmission
{
constexpr std::uint32_t QueueLimit(bool movieFrame)
{
    // The Bink decoder alternates two CPU-owned image buffers.
    return movieFrame ? 1u : 5u;
}

constexpr bool IsTarget(std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
{
    return title == 0x000500001010DD00ull &&
        ((version == 32 && rpx == 0x577202F4) || (version == 0 && rpx == 0xE426355B));
}

constexpr bool ShouldFlush(std::uint64_t title, std::uint32_t version, std::uint32_t rpx,
    bool mainCore, bool displayList, bool pendingCommands,
    std::uint32_t requested, std::uint32_t executed, bool movieFrame = false)
{
    const auto backlog = requested - executed;
    return IsTarget(title, version, rpx) && mainCore && !displayList && pendingCommands &&
        backlog > 0 && backlog < 0x80000000u && (movieFrame || backlog > 5);
}
}
