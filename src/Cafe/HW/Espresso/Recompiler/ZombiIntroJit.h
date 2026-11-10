#pragma once
#include <array>
#include <cstdint>

namespace ZombiIntroJit
{
constexpr std::array<std::uint32_t, 30> Entries{{
    0x02804cac, 0x02804e10, 0x028050d0, 0x02805a2c, 0x0280791c, 0x028079a8,
    0x02807d58, 0x028108d8, 0x02810c44, 0x02810c9c, 0x02810cb0, 0x02810cec,
    0x02810d14, 0x02811af0, 0x02811ecc, 0x028123d8, 0x02812948, 0x02813450,
    0x028134dc, 0x0281356c, 0x02813860, 0x02813a84, 0x02813b64, 0x02814d84,
    0x02815678, 0x0281568c, 0x028159bc, 0x02815cf4, 0x02818da8, 0x02818e48,
}};

constexpr bool IsDependency(std::uint64_t title, std::uint32_t version,
    std::uint32_t rpx, std::uint32_t address)
{
    if (title != 0x000500001010dd00ull || version != 32 || rpx != 0x577202f4 ||
        (address & 3) || address < Entries.front() || address > Entries.back())
        return false;
    // Only observed native decoder call targets, never arbitrary inner branches.
    for (const auto entry : Entries)
        if (entry == address)
            return true;
    return false;
}
}
