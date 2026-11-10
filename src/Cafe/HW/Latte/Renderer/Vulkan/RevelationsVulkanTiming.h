#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <utility>

namespace RevelationsVulkanTiming
{
constexpr bool Enabled(bool requested, std::uint64_t title, unsigned version, unsigned rpx)
{
    return requested && title == 0x000500001012cf00ull && version == 16 && rpx == 0xefa75829;
}

bool RuntimeEnabled();
enum class Stage : unsigned { Shader, Textures, Buffers, Pipeline, Descriptors, Submit, Count };
struct Frame
{
    std::array<std::uint64_t, static_cast<unsigned>(Stage::Count)> nanoseconds{};
    unsigned emitted{}, nullPipelines{}, skippedSequences{};
};
inline thread_local Frame frame;
inline thread_local Frame cumulative;

template<class Clock = std::chrono::steady_clock>
class Scope
{
public:
    Scope(Stage stage, bool enabled) : m_stage(stage), m_enabled(enabled)
    {
        if (m_enabled) m_start = Clock::now();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { Stop(); }
    void Stop()
    {
        if (!m_enabled) return;
        frame.nanoseconds[static_cast<unsigned>(m_stage)] +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start).count();
        m_enabled = false;
    }
private:
    Stage m_stage;
    bool m_enabled;
    typename Clock::time_point m_start{};
};

inline Frame TakeFrame()
{
    const auto result = std::exchange(frame, {});
    for (unsigned i = 0; i < result.nanoseconds.size(); ++i) cumulative.nanoseconds[i] += result.nanoseconds[i];
    cumulative.emitted += result.emitted;
    cumulative.nullPipelines += result.nullPipelines;
    cumulative.skippedSequences += result.skippedSequences;
    return result;
}
}
