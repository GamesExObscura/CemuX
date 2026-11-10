#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
namespace snd_core
{
    struct AxRemotePcm
    {
        static constexpr unsigned Capacity = 180;
        std::array<std::array<std::int16_t, Capacity>, 4> samples{};
        unsigned head{}, available{};
        void Push(const std::int16_t* frame)
        {
            if (available + 18 > Capacity) Advance(available + 18 - Capacity);
            for (unsigned i = 0; i < 18; ++i)
                for (unsigned channel = 0; channel < 4; ++channel)
                    samples[channel][(head + available + i) % Capacity] = frame[channel * 18 + i];
            available += 18;
        }
        std::int16_t Read(unsigned channel, unsigned offset) const { return samples[channel][(head + offset) % Capacity]; }
        void Advance(unsigned count) { count = std::min(count, available); head = (head + count) % Capacity; available -= count; }
    };
    inline std::array<std::int16_t, 18> ResampleRemote(std::span<const float> input)
    {
        std::array<std::int16_t, 18> output{};
        if (input.empty()) return output;
        for (unsigned o = 0; o < output.size(); ++o)
        {
            const unsigned begin = o * static_cast<unsigned>(input.size()), end = (o + 1) * static_cast<unsigned>(input.size());
            double sum = 0;
            for (unsigned i = begin / 18; i * 18 < end && i < input.size(); ++i)
            {
                const auto weight = std::min(end, (i + 1) * 18) - std::max(begin, i * 18);
                if (std::isfinite(input[i])) sum += input[i] * weight;
            }
            output[o] = static_cast<std::int16_t>(std::clamp(sum / input.size() / 256.0, -32768.0, 32767.0));
        }
        return output;
    }
}
