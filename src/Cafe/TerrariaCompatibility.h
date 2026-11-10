#pragma once
#include <cstdint>

namespace TerrariaCompatibility
{
constexpr bool Enabled(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x000500001019c300ull && version == 32 && rpx == 0x353154f1;
}
}
