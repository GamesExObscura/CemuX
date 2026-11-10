#pragma once
#include <cstdint>

namespace WheelAudioTiming
{
constexpr bool Enabled(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x0005000010111200ull && version == 0 && rpx == 0xf404d060;
}
}
