#pragma once
#include <cstdint>
namespace WatchDogsCachedState
{
constexpr bool Matches(std::uint64_t title, std::uint16_t version, std::uint32_t rpx)
{
    return title == 0x0005000010142000ull && version == 0 && rpx == 0x95fd128b;
}
template <typename State>
constexpr const State& Select(bool enabled, const State& captured, const State& live)
{
    return enabled ? captured : live;
}
}
