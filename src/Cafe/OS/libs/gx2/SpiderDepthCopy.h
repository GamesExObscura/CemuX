#pragma once
#include <cstdint>

namespace SpiderDepthCopy
{
constexpr bool Enabled(uint64_t title, uint32_t version, uint32_t rpx,
    uint32_t srcFormat, uint32_t dstFormat, uint32_t srcDim, uint32_t dstDim,
    uint32_t srcAA, uint32_t dstAA, uint32_t srcTile, uint32_t dstTile,
    uint32_t srcMip, uint32_t dstMip, uint32_t srcSlice, uint32_t dstSlice, uint32_t slices)
{
    return title == 0x000500001011c300ull && version == 0 && rpx == 0x8e00935b &&
        srcFormat == 0x11 && dstFormat == 0x80e && srcDim == 1 && dstDim == 1 &&
        srcAA == 0 && dstAA == 0 && srcTile == 4 && dstTile == 4 &&
        srcMip == 0 && dstMip == 0 && srcSlice == 0 && dstSlice == 0 && slices == 1;
}
}
