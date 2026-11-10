#pragma once

#include <cstdint>

namespace WatchDogsMipRelation
{
    constexpr bool IsTarget(std::uint64_t title, unsigned version, std::uint32_t rpx)
    {
        return title == 0x0005000010142000ull && version == 0 && rpx == 0x95fd128b;
    }

    enum class Parent { AddressOrder, First, Second };

    constexpr Parent SelectParent(bool enabled, std::uint32_t firstAddress,
                                  std::uint32_t secondAddress, int firstContainsMip,
                                  int secondContainsMip)
    {
        if (!enabled || firstAddress == secondAddress)
            return Parent::AddressOrder;
        if (firstContainsMip > 0 && secondContainsMip < 0)
            return Parent::First;
        if (secondContainsMip > 0 && firstContainsMip < 0)
            return Parent::Second;
        return Parent::AddressOrder;
    }
}
