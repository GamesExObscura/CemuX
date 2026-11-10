#pragma once
#include <cstdint>

namespace SplatoonLobbyLabels
{
constexpr bool MatchesTitle(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x0005000010176900ull && version == 288 && rpx == 0x9dbcf237;
}

constexpr bool IsPersistentFontDraw(uint64_t vs, uint64_t ps, uint32_t width, uint32_t height)
{
    return vs == 0xca58a2ebb6c263c8ull && ps == 0xd2282399b144eeffull &&
        ((width == 250 && height == 150) || (width == 128 && height == 32));
}

constexpr uint32_t BlockCount(uint32_t pixels)
{
    return pixels / 4 + (pixels % 4 != 0);
}

constexpr bool IsPartialBlockAlias(uint32_t srcFormat, uint32_t dstFormat,
    uint32_t srcWidth, uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight,
    uint32_t copyWidth, uint32_t copyHeight)
{
    return srcFormat == 0x122 && dstFormat == 0x35 &&
        srcWidth == 63 && srcHeight == 48 && dstWidth == 250 && dstHeight == 150 &&
        copyWidth == 62 && copyHeight == 37;
}
}
