#pragma once
#include <array>
#include <cstdint>

namespace WiiFitCursorBounds
{
constexpr bool IsTarget(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x0005000010102300ull && version == 81 && rpx == 0x61688449;
}
// HID::Initialize creates exactly two pointing devices. UICore::Read's third
// entry aliases the first object's flags (0x01100400), not another pointer.
constexpr std::array<uint32_t, 3> Reads{0x02393770, 0x0239380c, 0x0239383c};
constexpr uint32_t Expected = 0x80990008; // lwz r4,8(r25)
constexpr uint32_t AbsentDevice = 0x38800000; // li r4,0; existing null path handles absence
}
