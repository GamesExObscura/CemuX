#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

namespace GameListUpdates
{
template<class Title>
bool Enqueue(std::vector<Title>& pending, Title title)
{
    if (std::find(pending.begin(), pending.end(), title) != pending.end())
        return false;
    pending.push_back(title);
    return true;
}

template<class Clock = std::chrono::steady_clock>
class Slice
{
public:
    bool CanProcess() const
    {
        // Always make progress, even when one title takes longer than the budget.
        return m_count == 0 || (m_count < 8 && Clock::now() - m_start < std::chrono::milliseconds(4));
    }
    void DidProcess() { ++m_count; }
    std::size_t Count() const { return m_count; }
private:
    typename Clock::time_point m_start = Clock::now();
    std::size_t m_count = 0;
};
}
