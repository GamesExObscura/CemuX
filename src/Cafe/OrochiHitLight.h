#pragma once
#include <array>
#include <cstdint>

namespace OrochiHitLight
{
constexpr bool IsFiniteZeroDivide(double divisor, const std::array<uint32_t, 4>& color)
{
	if (divisor != 0.0)
		return false;
	for (const auto component : color)
		if ((component & 0x7f800000u) == 0x7f800000u)
			return false;
	return true;
}
}
