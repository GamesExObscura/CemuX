#pragma once
#include <array>
#include <cstdint>

namespace NbaNativeRowDispatch
{
constexpr uint32_t Native = 0x02fc3dc4, NativeSize = 0x6a4;
constexpr uint32_t NativeHash = 0x70d5da3e;
inline constexpr std::array<uint32_t, 18> Calls{{
    0x02fc4e4c,0x02fc4e58,0x02fc4e64,0x02fc4e70,0x02fc4e7c,
    0x02fc4e88,0x02fc4e94,0x02fc4ea0,0x02fc4ec0,0x02fc542c,
    0x02fc5438,0x02fc5444,0x02fc5450,0x02fc545c,0x02fc5468,
    0x02fc5474,0x02fc5480,0x02fc54a0}};
}
