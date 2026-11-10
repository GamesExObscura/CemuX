#pragma once
#include <cstdint>

namespace nn::act::detail
{
    template<typename Lookup>
    constexpr std::uint8_t FindSlotForPersistentId(std::uint32_t persistentId, Lookup lookup)
    {
        if (persistentId == 0)
            return 0;
        for (std::uint8_t slot = 1; slot <= 12; ++slot)
        {
            if (lookup(slot) == persistentId)
                return slot;
        }
        return 0;
    }
}
