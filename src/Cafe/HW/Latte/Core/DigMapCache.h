#pragma once
#include <cstdint>

namespace DigMapCache
{
constexpr bool NeedsFullHash(uint64_t title, uint32_t version, uint32_t rpx,
    uint32_t width, uint32_t height, uint32_t depth, uint32_t format,
    uint32_t tileMode, uint32_t pitch, uint32_t bytes, bool gpuWritten)
{
    // Three CPU map banks acquire small path edits between sparse hash samples.
    return title == 0x000500001018f100ull && version == 0 && rpx == 0xcefe8b82 &&
        width == 32 && height == 64 && depth == 1 && format == 0xb &&
        tileMode == 1 && pitch == 128 && bytes == 16384 && !gpuWritten;
}
}
