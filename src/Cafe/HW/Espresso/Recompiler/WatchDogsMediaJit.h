#pragma once
#include <cstdint>

namespace WatchDogsMediaJit
{
constexpr uint32_t decoderEntries[] = {
    0x0675f304, 0x0675f64c, 0x0675f66c, 0x0675f68c, 0x0675f6f8,
    0x0675f814, 0x0675f850, 0x0675f88c, 0x0675fa60, 0x0675fabc,
    0x0675fb5c, 0x0675fbf8, 0x0675fc24, 0x0675fd24, 0x0675fda4,
    0x0675ff0c, 0x067600b4, 0x067600d0, 0x067600ec, 0x06760108,
    0x06760124, 0x06760148, 0x0676016c, 0x0676018c, 0x067601a8,
    0x067601cc, 0x06760314, 0x06760334, 0x06760350, 0x06760374,
    0x06760394, 0x067603b4, 0x067603f0, 0x067605ac, 0x067606f0,
    0x067608f8, 0x06760938, 0x06760978, 0x06760dc0, 0x06760e44,
    0x06761508, 0x06763648, 0x0676405c, 0x0676408c, 0x067648c0,
    0x06764d28, 0x06764e50, 0x06765024, 0x067652f4, 0x06765438,
    0x06765b8c, 0x06765f00, 0x06765f48, 0x0676612c, 0x0676626c,
    0x06766a6c, 0x06766aac, 0x06766b94, 0x0676e4d4, 0x0676e894,
    0x0676e91c, 0x0676f7e8, 0x0676fc00, 0x06779f30,
};

constexpr bool IsDependency(uint64_t title, uint32_t version, uint32_t hash, uint32_t address)
{
    if ((address & 3) || address < 0x0675f304 || address > 0x06779f30 ||
        title != 0x0005000010142000ull || version != 0 || hash != 0x95fd128b)
        return false;
    // Promote only requested decoder function entries, not their inner branches:
    // initialization branches otherwise exhaust the bounded priority queue.
    for (const auto entry : decoderEntries)
        if (entry == address)
            return true;
    return false;
}
}
