#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

class AudioOutputVolume
{
public:
    std::int32_t Requested() const { return m_requested.load(); }
    bool Muted() const { return m_muted.load(); }
    template<class Apply> void Set(std::int32_t volume, Apply apply)
    {
        std::lock_guard lock(m_mutex);
        m_requested.store(volume);
        apply(m_muted.load() ? 0 : volume);
    }
    template<class Apply> void Mute(bool muted, Apply apply)
    {
        if (m_muted.load() == muted) return;
        std::lock_guard lock(m_mutex);
        if (m_muted.exchange(muted) == muted) return;
        apply(muted ? 0 : m_requested.load());
    }
private:
    std::mutex m_mutex;
    std::atomic<std::int32_t> m_requested{0};
    std::atomic<bool> m_muted{false};
};
