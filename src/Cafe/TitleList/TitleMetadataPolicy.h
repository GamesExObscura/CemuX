#pragma once

#include <cstdint>
#include <string_view>

namespace TitleMetadataPolicy
{
// Factory layout data is not runnable software and intentionally has no meta.xml.
constexpr bool IsFactoryLayout(std::uint64_t titleId, std::uint32_t appType, std::string_view argument)
{
    return (titleId >> 32) == 0x0005000B && appType == 0x0800002A && argument == ".rpx";
}
}
