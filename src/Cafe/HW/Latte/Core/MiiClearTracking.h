#pragma once
#include <cstdint>

namespace MiiClearTracking
{
constexpr bool NeedsFreshBaseline(uint64_t title, unsigned version, uint32_t rpx,
    unsigned format, unsigned width, unsigned height, unsigned depth,
    unsigned pitch, unsigned mipLevels, unsigned tileMode)
{
    return title == 0x000500101004a100ULL && version == 50 && rpx == 0xdfe380d3 &&
        format == 0x1a && height == 256 && depth == 1 && mipLevels == 1 &&
        tileMode == 4 && ((width == 128 && pitch == 128) || (width == 256 && pitch == 256));
}
}
