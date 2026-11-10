#pragma once
#include <cstdint>

namespace RacingCommandBatch
{
    constexpr bool Applies(uint64_t title, uint32_t version, uint32_t rpx, uint32_t poolWords)
    {
        return title == 0x000500001010b300ull && version == 48 &&
            rpx == 0x7354e64b && poolWords >= 0x8000;
    }
}
