#pragma once
#include <cstdint>

namespace StarFoxNativeDepth
{
constexpr bool IsTarget(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x00050000101b0400ull && version == 16 && rpx == 0x3768054d;
}

constexpr bool UseNativeDepth(bool supported, bool hasGraphicPacks, uint64_t title, uint32_t version, uint32_t rpx)
{
    return supported && !hasGraphicPacks && IsTarget(title, version, rpx);
}

constexpr uint64_t ShaderKey(uint64_t key, bool enabled)
{
    return enabled ? key ^ 0x53465a4e44433031ull : key;
}
}
