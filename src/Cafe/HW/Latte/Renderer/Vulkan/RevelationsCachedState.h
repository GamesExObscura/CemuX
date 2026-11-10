#pragma once
#include <cstdint>

namespace RevelationsCachedState
{
constexpr bool Matches(std::uint64_t title, std::uint16_t version, std::uint32_t rpx)
{
    return title == 0x000500001012cf00ull && version == 16 && rpx == 0xefa75829;
}

template <typename State>
constexpr const State& Select(bool correctCache, const State& restored, const State& live)
{
    return correctCache ? restored : live;
}
}
