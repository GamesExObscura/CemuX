#pragma once
#include <cstdint>

namespace SmurfsParticleSampler
{
    constexpr bool UseRowPrecision(uint64_t title, uint32_t version, uint32_t rpx,
        uint64_t shader)
    {
        return title == 0x0005000010113300ull && version == 0 && rpx == 0xaf0f5c08 &&
            (shader == 0x66b64ea87e9bc80bull || shader == 0x4b4060d76b496cb3ull ||
             shader == 0xd8937de66d4b07b9ull);
    }

    constexpr uint64_t CacheSalt = 0x534d555246530001ull;
}
