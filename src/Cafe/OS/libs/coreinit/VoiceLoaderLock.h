#pragma once
#include <cstdint>

namespace VoiceLoaderLock
{
constexpr bool NeedsSchedulingTurn(std::uint64_t title, std::uint16_t version,
    std::uint32_t rpx, std::uint32_t mutexAddress, bool ownsMutex,
    std::int32_t lockCount, bool hasWaiter)
{
    return title == 0x0005000010183300ull && version == 0 && rpx == 0x82f69949 &&
        mutexAddress == 0x101066d8 && ownsMutex && lockCount == 1 && hasWaiter;
}
}
