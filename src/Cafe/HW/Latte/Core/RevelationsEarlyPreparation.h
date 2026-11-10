#pragma once
#include <cstdint>
#include <string_view>

namespace RevelationsEarlyPreparation
{
constexpr unsigned MaxPending = 512;
constexpr unsigned MaxPrograms = 4;
constexpr unsigned MaxBinaryBytes = 64 * 1024 * 1024;
constexpr unsigned MaxProgramBytes = 64 * 1024;
constexpr bool Enabled(bool requested, bool openGL, bool parallel,
    std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
{
    return requested && openGL && parallel && title == 0x000500001012cf00ull &&
        version == 16 && rpx == 0xefa75829;
}
constexpr bool ValidProgram(unsigned bytes)
{
    return bytes >= 8 && bytes <= MaxProgramBytes && bytes % 8 == 0;
}
constexpr bool SameSource(unsigned preparedType, std::string_view prepared,
    unsigned actualType, std::string_view actual)
{
    return preparedType == actualType && !prepared.empty() && prepared == actual;
}
constexpr int SamplerDimension(unsigned type)
{
    // WUT GX2SamplerVarType -> GX2SurfaceDim; array/format predictions remain conservative.
    switch (type) { case 0: return 0; case 1: return 1; case 3: return 2; case 4: return 3; default: return -1; }
}
}

