#pragma once
#include <cstdint>

namespace BlacklistSurfaceCopy
{
constexpr bool IsTitle(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x000500001010e300ull && version == 48 && rpx == 0x36857ada;
}

constexpr uint32_t DimensionBridgeTexelBytes(uint32_t format)
{
    switch (format)
    {
    case 0x806: return 2; // R16_FLOAT
    case 0x1a: return 4; // RGBA8_UNORM
    case 0x820: return 8; // RGBA16_FLOAT
    default: return 0;
    }
}

constexpr bool NeedsDimensionBridge(uint64_t title, uint32_t version, uint32_t rpx,
    uint32_t srcDim, uint32_t dstDim, uint32_t srcFormat, uint32_t dstFormat,
    uint32_t width, uint32_t height, uint32_t depth)
{
    return IsTitle(title, version, rpx) &&
        ((srcDim == 1 && dstDim == 0) || (srcDim == 0 && dstDim == 1)) &&
        srcFormat == dstFormat && DimensionBridgeTexelBytes(srcFormat) != 0 && width > 0 && width <= 16384 &&
        height == 1 && depth == 1;
}

constexpr bool IsVisibilityReadback(uint64_t title, uint32_t version, uint32_t rpx,
    uint32_t dim, uint32_t format, uint32_t tile, uint32_t width, uint32_t height,
    uint32_t pitch, uint32_t mip, uint32_t slice)
{
    return title == 0x000500001010e300ull && version == 48 && rpx == 0x36857ada &&
        dim == 1 && format == 1 && tile == 1 && width == 256 && height == 128 &&
        pitch >= width && pitch <= 16384 && mip == 0 && slice == 0;
}

constexpr uint32_t ReadbackBytes(uint64_t title, uint32_t version, uint32_t rpx,
    uint32_t dim, uint32_t format, uint32_t width, uint32_t height,
    uint32_t mip, uint32_t slice)
{
    if (title != 0x000500001010e300ull || version != 48 || rpx != 0x36857ada ||
        dim > 1 || format != 0x806 || width == 0 || height == 0 ||
        width > 16384 || height > 16384 || (dim == 0 && height != 1) || mip != 0 || slice != 0)
        return 0;
    return width * height * 2;
}

constexpr bool IsOneDimensionalCopy(uint64_t title, uint32_t version, uint32_t rpx,
    uint32_t srcDim, uint32_t dstDim, uint32_t srcFormat, uint32_t dstFormat,
    uint32_t width, uint32_t height, uint32_t srcSlice, uint32_t dstSlice,
    uint32_t srcAA, uint32_t dstAA)
{
    return title == 0x000500001010e300ull && version == 48 && rpx == 0x36857ada &&
        srcDim == 1 && dstDim == 0 && srcFormat == 0x806 && dstFormat == 0x806 &&
        width == 512 && height == 1 && srcSlice == 0 && dstSlice == 0 && srcAA == 0 && dstAA == 0;
}
}
