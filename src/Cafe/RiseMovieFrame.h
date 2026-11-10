#pragma once
#include <cstdint>

namespace RiseMovieFrame
{
    inline bool IsTarget(uint64_t title, uint32_t version, uint32_t rpx)
    {
        return title == 0x000500001014c100ull && version == 0 && rpx == 0xfab814f1;
    }

    inline bool NeedsRefresh(uint32_t frame, uint32_t completed, uint32_t previous,
        uint32_t replacement, uint32_t count, uint32_t width, uint32_t height)
    {
        return frame == 1 && completed == 1 && previous && replacement &&
            previous != replacement && count == 2 && width == 1280 && height == 720;
    }
}
