#pragma once
#include <cstddef>
#include <cstdint>

namespace ZombiDeviceMixMask
{
    constexpr size_t BusCount = 4;

    constexpr bool Enabled(uint64_t title, uint32_t version, uint32_t hash)
    {
        return title == 0x000500001010dd00ull &&
            ((version == 0 && hash == 0xe426355b) ||
             (version == 32 && hash == 0x577202f4));
    }

    constexpr size_t DrcMaskOffset(uint64_t title, uint32_t version,
        uint32_t hash, int deviceIndex)
    {
        // Each DRC has its own four bus masks, just as its sample gains do.
        return Enabled(title, version, hash) && deviceIndex == 1 ? BusCount : 0;
    }
}
