#pragma once
#include <cstdint>

namespace RunbowCompatibility
{
constexpr bool NeedsFullTextureHash(uint64_t title, uint32_t version, uint32_t rpxHash,
    uint32_t format, bool gpuWritten)
{
    // Runbow reuses CPU sprite/letter atlases; sparse samples miss their updates.
    return title == 0x0005000010183000ull && version == 80 && rpxHash == 0x59076f4c &&
        !gpuWritten && (format == 0x33 || format == 0x01); // BC3_UNORM or R8_UNORM
}
}
