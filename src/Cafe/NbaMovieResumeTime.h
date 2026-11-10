#pragma once
#include <cstdint>
#include <mutex>

namespace NbaMovieResumeTime
{
struct Sample
{
    std::uint32_t delta{};
    std::uint64_t suspendedMs{};
    bool resumed{}, corrected{};
};
class Recovery
{
public:
    void Reset() { std::lock_guard lock(mutex); active = pending = 0; start = 0; }
    void NoteMovie(std::uint32_t root) { std::lock_guard lock(mutex); active = root; }
    void Suspend(std::uint64_t ticks) { std::lock_guard lock(mutex); pending = active; start = ticks; }
    Sample FirstUpdate(std::uint32_t root, std::uint32_t delta, std::uint64_t now, bool valid)
    {
        std::lock_guard lock(mutex);
        Sample sample{delta};
        if (!pending || root != pending) return sample;
        pending = 0; sample.resumed = true;
        if (!valid || now < start) return sample;
        // Match the original caller's OSGetTime alignment/division literal.
        sample.suspendedMs = now / 0xf2cc - start / 0xf2cc;
        // The observed reacquire failure supplies a multi-billion-ms interval.
        // Recover only that impossible first step; never clamp ordinary updates.
        if (static_cast<std::uint64_t>(delta) > sample.suspendedMs + 1000)
        {
            sample.delta = 0;
            sample.corrected = true;
        }
        return sample;
    }
private:
    std::mutex mutex;
    std::uint32_t active{}, pending{};
    std::uint64_t start{};
};
inline Recovery recovery;
}
