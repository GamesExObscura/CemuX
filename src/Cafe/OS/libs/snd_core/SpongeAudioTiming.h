#pragma once
#include <cstdint>

namespace SpongeAudioTiming
{
constexpr bool Enabled(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x0005000010146900ull && version == 0 && rpx == 0x1bfbf952;
}
}
