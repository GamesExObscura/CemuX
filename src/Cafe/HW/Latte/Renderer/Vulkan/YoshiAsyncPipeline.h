#pragma once

#include <cstdint>

namespace YoshiAsyncPipeline
{
    inline bool Matches(std::uint64_t title, std::uint32_t version, std::uint32_t hash)
    {
        return title == 0x0005000010184d00ull && version == 32 && hash == 0x89872752;
    }

    inline bool PreserveDraw(std::uint32_t width, std::uint32_t height,
        std::uint32_t indices, bool hasDepth)
    {
        // Character thumbnails are baked once, unlike the continuously redrawn scene.
        return (width == 160 && height == 208) || indices <= 6 || !hasDepth;
    }
}
