#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>

namespace HobbitMapWorkaround
{
inline constexpr std::string_view PackPath = "LEGO The Hobbit/Workarounds/Map Lighting Stabilization";

inline bool MatchesTrack(uint32_t count, std::string_view owner, uint16_t firstValue,
    uint32_t firstTime, uint16_t lastValue, uint32_t lastTime)
{
    return count == 135 && owner == "DynamicLightDesc.LightDescription70Anim" &&
        firstValue == 0x3370 && firstTime == 1 && lastValue == 0x341a && lastTime == 537;
}

// Optional steady-map compatibility workaround, not a hardware-accuracy correction.
// Keep the identified light near its observed midrange without animated pulses.
struct Filter
{
    float Apply(uint32_t, double newTime, float sample) const
    {
        if (!std::isfinite(newTime) || !std::isfinite(sample) || sample < 0.19f || sample > 0.31f)
            return sample;
        return 0.25f;
    }
};
}
