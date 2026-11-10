#pragma once
#include <cstdint>
#include <atomic>
#include <chrono>
#include "NbaHomeMovieTiming.h"
#include "NbaHomeCheck.h"

namespace NbaHomeAudio
{
    constexpr bool Enabled(std::uint64_t title, std::uint32_t version, std::uint32_t hash)
    {
        return title == 0x000500001010da00ULL && version == 32 && hash == 0xef62d66e;
    }
    constexpr bool MixerMayRun(bool suspended, bool released, bool nba)
    {
        return !suspended || (nba && !released);
    }
    inline std::atomic<std::int64_t> traceUntilNs{};
    inline std::int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    inline void BeginResumeTrace() { NbaHomeMovieTiming::counters.Reset(); traceUntilNs.store(NowNs() + 6'000'000'000LL); }
    inline bool TraceResume() { const auto until = traceUntilNs.load(); return until && NowNs() < until; }
    inline bool TraceMovie() { return NbaHomeCheck::Enabled() || TraceResume(); }
}
