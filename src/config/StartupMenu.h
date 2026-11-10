#pragma once
#include <cstdint>

namespace StartupMenu
{
    constexpr std::uint64_t MenuTitleForRegion(std::uint32_t region)
    {
        return region == 1 ? 0x0005001010040000ULL :
            (region == 4 || region == 8) ? 0x0005001010040200ULL : 0x0005001010040100ULL;
    }

    constexpr bool ShouldAutoStart(bool enabled, bool explicitGame, bool explicitTitle, bool suppressed, bool helper)
    {
        return enabled && !explicitGame && !explicitTitle && !suppressed && !helper;
    }
}
