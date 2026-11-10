#pragma once

#include <bit>
#include <cstdint>

namespace PipelineBlendHash
{
constexpr uint64_t Mix(uint64_t stateHash, uint32_t blendControl)
{
    return std::rotl(stateHash, 11) ^ (uint64_t(blendControl) * 0x9e3779b185ebca87ull);
}
}
