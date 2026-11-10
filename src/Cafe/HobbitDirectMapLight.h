#pragma once
#include <cmath>
#include <cstdint>
#include <string_view>

namespace HobbitDirectMapLight
{
// Direct compatibility workaround, not a proven renderer/hardware correction.
constexpr bool IsTarget(uint64_t title, unsigned version, uint32_t rpx,
    std::string_view module, uint32_t crc)
{
    return title == 0x000500001016a700ULL && version == 0 && rpx == 0xfa868d52 &&
        module == "hobbit" && crc == 0xad4603b3;
}

constexpr bool MatchesTrack(uint32_t count, std::string_view owner,
    uint16_t firstValue, uint32_t firstTime, uint16_t lastValue, uint32_t lastTime)
{
    return count == 135 && owner == "DynamicLightDesc.LightDescription70Anim" &&
        firstValue == 0x3370 && firstTime == 1 && lastValue == 0x341a && lastTime == 537;
}

struct Filter
{
    static constexpr double ResponseFrames = 45.0;
    uint32_t track = 0;
    double time = 0;
    double value = 0;
    bool initialized = false;

    float Apply(uint32_t newTrack, double newTime, float sample)
    {
        if (!std::isfinite(newTime) || !std::isfinite(sample) || sample < 0.19f || sample > 0.31f)
        {
            initialized = false;
            return sample;
        }
        if (!initialized || newTrack != track || newTime < time || newTime - time > 60.0)
        {
            initialized = true;
            track = newTrack;
            time = newTime;
            value = sample;
            return sample;
        }
        if (newTime == time)
            return static_cast<float>(value);
        // Advance by authored animation time, not number of host draws/calls.
        const double alpha = -std::expm1(-(newTime - time) / ResponseFrames);
        value += alpha * (static_cast<double>(sample) - value);
        time = newTime;
        return static_cast<float>(value);
    }
};
}
