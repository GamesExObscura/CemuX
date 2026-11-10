#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace NbaHomeCheck
{
enum class Mode { Off, Observe, ParentCycle };
constexpr Mode Parse(std::string_view value)
{
    if (value == "observe") return Mode::Observe;
    if (value == "parent-cycle") return Mode::ParentCycle;
    return Mode::Off;
}
inline Mode CurrentMode()
{
    return Mode::Off;
}
inline bool Enabled() { return CurrentMode() != Mode::Off; }
inline std::atomic<std::uint64_t> movieFrames{};
inline std::uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
class Scenario
{
public:
    bool RequestRelease(Mode mode, bool targetAllowed, bool idle, std::uint64_t frames)
    {
        if (mode != Mode::ParentCycle || !targetAllowed || !idle || requested || frames < 150) return false;
        requested = true;
        return true;
    }
    bool Requested() const { return requested; }
    void Released(std::uint64_t now) { if (requested && !released) { released = true; releasedAt = now; } }
    bool RequestResume(std::uint64_t now)
    {
        if (!released || resumed || now < releasedAt || now - releasedAt < 3'000'000'000ULL) return false;
        resumed = true;
        return true;
    }
private:
    bool requested{}, released{}, resumed{};
    std::uint64_t releasedAt{};
};
inline Scenario scenario;
}
