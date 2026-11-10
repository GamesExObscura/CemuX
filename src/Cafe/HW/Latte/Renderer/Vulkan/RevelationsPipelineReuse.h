#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace RevelationsPipelineReuse
{
constexpr bool Matches(bool requested, uint64_t title, uint32_t version, uint32_t rpx)
{
    return requested && title == 0x000500001012cf00ull && version == 16 && rpx == 0xefa75829;
}

struct Key
{
    uint64_t base{}, state{};
    bool operator==(const Key&) const = default;
    struct Hash
    {
        size_t operator()(const Key& key) const { return static_cast<size_t>(key.base ^ key.state); }
    };
};

class Claims
{
public:
    struct Stats { size_t records{}, unique{}, duplicates{}; };
    bool Observe(Key key)
    {
        std::lock_guard lock(m_mutex);
        ++m_records;
        m_observed.insert(key);
        return m_claimed.insert(key).second;
    }
    void Failed(Key key)
    {
        std::lock_guard lock(m_mutex);
        m_claimed.erase(key);
    }
    Stats Snapshot()
    {
        std::lock_guard lock(m_mutex);
        return {m_records, m_observed.size(), m_records - m_observed.size()};
    }
    void Reset()
    {
        std::lock_guard lock(m_mutex);
        m_observed.clear();
        m_claimed.clear();
        m_records = 0;
    }
private:
    std::mutex m_mutex;
    std::unordered_set<Key, Key::Hash> m_observed, m_claimed;
    size_t m_records{};
};
}
