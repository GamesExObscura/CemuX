#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace NbaHomeMovieTiming
{
enum class Path { Native, Fast, Parity };
struct Snapshot
{
    std::uint64_t calls{}, fast{}, native{}, parity{}, totalNs{}, maxNs{};
    std::uint64_t swaps{}, swapNs{}, maxSwapNs{};
    std::uint64_t rows{}, rowNs{}, maxRowNs{}, movieSamples{};
    std::uint32_t nativeIndexFirst{}, nativeIndexLast{};
    std::int32_t fallbackPitch{};
    std::uint32_t fallbackGqr{}, focusedPid{}, ownerPid{};
};
class Counters
{
public:
    void Reset() { std::lock_guard lock(m_mutex); ++m_generation; m_data = {}; }
    std::uint64_t Generation() { std::lock_guard lock(m_mutex); return m_generation; }
    void Record(std::uint64_t generation, Path path, std::uint64_t ns,
        std::int32_t pitch, std::uint32_t gqr)
    {
        std::lock_guard lock(m_mutex);
        if (generation != m_generation) return;
        ++m_data.calls;
        if (path == Path::Fast) ++m_data.fast;
        else if (path == Path::Parity) ++m_data.parity;
        else { ++m_data.native; m_data.fallbackPitch = pitch; m_data.fallbackGqr = gqr; }
        m_data.totalNs += ns; m_data.maxNs = std::max(m_data.maxNs, ns);
    }
    void RecordSwap(std::uint64_t generation, std::uint64_t ns)
    {
        std::lock_guard lock(m_mutex);
        if (generation != m_generation) return;
        ++m_data.swaps; m_data.swapNs += ns; m_data.maxSwapNs = std::max(m_data.maxSwapNs, ns);
    }
    void RecordRow(std::uint64_t generation, std::uint64_t ns)
    {
        std::lock_guard lock(m_mutex);
        if (generation != m_generation) return;
        ++m_data.rows; m_data.rowNs += ns; m_data.maxRowNs = std::max(m_data.maxRowNs, ns);
    }
    void RecordMovie(std::uint32_t nativeIndex)
    {
        std::lock_guard lock(m_mutex);
        if (!m_data.movieSamples++) m_data.nativeIndexFirst = nativeIndex;
        m_data.nativeIndexLast = nativeIndex;
    }
    Snapshot Take()
    {
        std::lock_guard lock(m_mutex);
        const auto result = m_data; m_data = {}; return result;
    }
    void Focus(std::uint32_t focusedPid, std::uint32_t ownerPid)
    {
        std::lock_guard lock(m_mutex); m_data.focusedPid = focusedPid; m_data.ownerPid = ownerPid;
    }
private:
    std::mutex m_mutex;
    std::uint64_t m_generation{1};
    Snapshot m_data;
};
inline Counters counters;
inline std::uint64_t NowNs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
class Sample
{
public:
    Sample(bool enabled, std::int32_t pitch, std::uint32_t gqr) : m_enabled(enabled), m_pitch(pitch), m_gqr(gqr)
    {
        if (m_enabled) { m_generation = counters.Generation(); m_begin = NowNs(); }
    }
    ~Sample() { if (m_enabled) counters.Record(m_generation, m_path, NowNs() - m_begin, m_pitch, m_gqr); }
    void SetPath(Path path) { m_path = path; }
    Sample(const Sample&) = delete;
    Sample& operator=(const Sample&) = delete;
private:
    bool m_enabled;
    Path m_path{Path::Native};
    std::int32_t m_pitch;
    std::uint32_t m_gqr;
    std::uint64_t m_generation{}, m_begin{};
};
}
