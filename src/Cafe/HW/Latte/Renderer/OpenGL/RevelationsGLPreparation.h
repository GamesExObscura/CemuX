#pragma once
#include <cstdint>

namespace RevelationsGLPreparation
{
constexpr bool QueueBeforeWait(bool enabled, bool loading, bool gameShader, bool customShader,
    std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
{
    return enabled && loading && gameShader && !customShader && title == 0x000500001012cf00ull &&
        version == 16 && rpx == 0xefa75829;
}
}
