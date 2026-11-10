#pragma once
#include <cstdint>

namespace DigAudioTiming
{
constexpr bool Enabled(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x000500001018f100ull && version == 0 && rpx == 0xcefe8b82;
}
}
