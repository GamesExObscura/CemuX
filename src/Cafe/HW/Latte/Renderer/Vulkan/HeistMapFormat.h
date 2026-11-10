#pragma once
#include <cstdint>

namespace HeistMapFormat
{
constexpr bool IsTarget(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x00050000101a4000ull && version == 0 && rpx == 0xf801037e;
}

constexpr bool NeedsFallback(uint64_t title, uint32_t version, uint32_t rpx,
    bool colorAttachment, bool blending)
{
    return IsTarget(title, version, rpx) && (!colorAttachment || !blending);
}
}
