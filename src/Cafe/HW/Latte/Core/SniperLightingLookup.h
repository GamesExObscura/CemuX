#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace SniperLightingLookup
{
    constexpr bool Matches(uint64_t title, uint32_t version, uint32_t rpx,
        uint32_t format, uint32_t width, uint32_t height, uint32_t depth,
        uint32_t pitch, uint32_t mipLevels, uint32_t tileMode)
    {
        return title == 0x0005000010134e00ull && version == 1 && rpx == 0x056cec3a &&
            format == 0x1f && width == 256 && height == 1024 && depth == 1 &&
            pitch == 256 && mipLevels == 1 && tileMode == 4;
    }

    inline void SwapComponents(uint8_t* data, size_t size)
    {
        for (size_t i = 0; i + 1 < size; i += 2)
            std::swap(data[i], data[i + 1]);
    }
}
