#pragma once

#include <array>
#include <cstdint>

namespace WatchDogsVisibility
{
	constexpr std::array<uint32_t, 7> Addresses{
		0x057fd8bc, 0x057fd8cc, 0x057fd8d4, 0x057fd8d8,
		0x057fd910, 0x057fd9cc, 0x057fd9f4};
	constexpr std::array<uint32_t, 7> Expected{
		0x3be00000, 0x4bffdda1, 0x40820008, 0x3be00001,
		0x2c1f0000, 0x4bfff6c5, 0x4bfffa69};

	constexpr bool Matches(uint64_t title, uint32_t version, uint32_t rpx,
		const std::array<uint32_t, 7>& words)
	{
		return title == 0x0005000010142000ull && version == 0 &&
			rpx == 0x95fd128b && words == Expected;
	}
}
