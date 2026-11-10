#pragma once
#include <cstdint>

namespace Spider2GamePadTarget
{
    constexpr bool PreserveHeight(uint64_t title, uint32_t version, uint32_t rpx,
        uint32_t pitch, uint32_t height, uint32_t format, uint32_t tileMode)
    {
        return title == 0x0005000010146a00ull && version == 0 && rpx == 0x5b546d51 &&
            pitch == 864 && height == 480 && format == 0x41a && tileMode == 4;
    }
}
