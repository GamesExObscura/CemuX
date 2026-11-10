#pragma once
#include <cstdint>

namespace ZumbaLoaderLock
{
constexpr bool NeedsSchedulingTurn(std::uint64_t title, std::uint16_t version,
    std::uint32_t rpx, std::uint32_t mutexAddress, bool ownsMutex,
    std::int32_t lockCount, bool hasWaiter)
{
    return title == 0x0005000010142a00ull && version == 16 && rpx == 0x0babef5b &&
        mutexAddress == 0x10131770 && ownsMutex && lockCount == 1 && hasWaiter;
}
}
