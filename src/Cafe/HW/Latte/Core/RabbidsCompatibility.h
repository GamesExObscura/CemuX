#pragma once
#include <cstdint>

namespace RabbidsCompatibility
{
constexpr bool WaitForQueriesBeforeRetirement(uint64_t title, uint32_t version,
    uint32_t rpxHash, bool vulkan)
{
    return title == 0x000500001010b000ull && version == 16 && rpxHash == 0xb92e71ae && vulkan;
}

constexpr bool UseAttachmentlessStreamout(uint64_t title, uint32_t version, uint32_t rpxHash,
    bool vulkan, bool streamout, bool rasterizerKill)
{
    return title == 0x000500001010b000ull && version == 16 && rpxHash == 0xb92e71ae &&
        vulkan && streamout && rasterizerKill;
}
}
