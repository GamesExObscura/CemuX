#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nsyshid::ghl
{
	constexpr uint16_t VendorId = 0x12ba;
	constexpr uint16_t ProductId = 0x074b;

	struct Input
	{
		std::array<bool, 3> white{};
		std::array<bool, 3> black{};
		bool strumUp{}, strumDown{}, left{}, right{};
		bool start{}, heroPower{}, tv{};
		uint8_t whammy{0x80};
		uint8_t tilt{0x80};
	};

	// PS3/Wii U receiver wire layout, cross-checked against RPCS3's GHLtar protocol.
	constexpr std::array<uint8_t, 32> Encode(const Input& input)
	{
		std::array<uint8_t, 32> report{};
		constexpr std::array<uint8_t, 3> whiteBits{0x01, 0x10, 0x20};
		constexpr std::array<uint8_t, 3> blackBits{0x02, 0x04, 0x08};
		for (std::size_t i = 0; i < 3; ++i)
		{
			if (input.white[i]) report[0] |= whiteBits[i];
			if (input.black[i]) report[0] |= blackBits[i];
		}
		report[1] = (input.heroPower ? 1 : 0) | (input.start ? 2 : 0) | (input.tv ? 4 : 0);
		report[2] = input.left == input.right ? 0x0f : (input.left ? 2 : 6);
		report[3] = 0x80;
		report[4] = input.strumUp == input.strumDown ? 0x80 : (input.strumUp ? 0 : 0xff);
		report[5] = input.tilt >= 0xf0 ? 0xff : (input.tilt <= 0x10 ? 0 : 0x80);
		report[6] = input.whammy;
		report[19] = input.tilt;
		report[22] = 1;
		report[24] = report[26] = 2;
		return report;
	}
}
