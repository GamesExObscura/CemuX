#pragma once
#include <cstdint>

namespace BoomGamePadCopy
{
constexpr bool Matches(uint64_t title, uint32_t version, uint32_t rpx,
    uint32_t srcFormat, uint32_t dstFormat, uint32_t width, uint32_t height,
    uint32_t srcPitch, uint32_t dstPitch, uint32_t srcTile, uint32_t dstTile,
    uint32_t srcAA, uint32_t dstAA, uint32_t srcSlice, uint32_t dstSlice)
{
    return title == 0x0005000010175b00ull && version == 16 && rpx == 0x6df940ae &&
        srcFormat == 0x1a && dstFormat == 0x19 && width == 1280 && height == 720 &&
        srcPitch == 1280 && dstPitch == 1280 && srcTile == 4 && dstTile == 4 &&
        srcAA == 0 && dstAA == 0 && srcSlice == 0 && dstSlice == 0;
}
}
