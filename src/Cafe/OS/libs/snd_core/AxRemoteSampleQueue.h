#pragma once
#include <algorithm>
#include <cstdint>

namespace snd_core
{
// Silent compatibility sink: the current remote final-mix contract is 18 samples
// per 3 ms AX frame. A bounded queue lets SDK control callbacks keep progressing.
struct AxRemoteSampleQueue
{
    static constexpr uint32_t Capacity = 180;
    uint32_t last_frame = 0;
    uint32_t available = 0;

    void Update(uint32_t frame)
    {
        const uint32_t elapsed = frame - last_frame;
        last_frame = frame;
        available = static_cast<uint32_t>(std::min<uint64_t>(Capacity, available + uint64_t{elapsed} * 18));
    }

    void Advance(uint32_t count) { available -= std::min(count, available); }
};
}
