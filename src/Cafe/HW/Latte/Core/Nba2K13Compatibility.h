#pragma once
#include <cstdint>

namespace Nba2K13Compatibility
{
constexpr bool IsTarget(uint64_t title, uint32_t version, uint32_t hash)
{
	return title == 0x000500001010da00ull && version == 32 && hash == 0xef62d66e;
}

constexpr bool IsBoardApertureLayout(uint32_t width, uint32_t height, uint32_t format, uint32_t levels)
{
	return width == 512 && height == 256 && (format == 0x31 || format == 0x431) && levels == 6;
}

constexpr uint32_t BoardApertureBytes(uint32_t blockPitch, uint32_t texelHeight)
{
	return blockPitch * ((texelHeight + 3) / 4) * 8;
}

constexpr bool IsJerseyApertureLayout(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, uint32_t mip)
{
	return (format == 0x1a || format == 0x41a) && mip > 0 && mip < levels &&
		((width == 512 && height == 64 && levels == 5) || (width == 256 && height == 128 && levels == 4));
}

constexpr bool IsGeneratedPlayerMaterial(uint64_t shader, uint32_t unit)
{
	return (unit == 2 && (shader == 0x1b91a8db1a9eabbaull || shader == 0x23fca41503869299ull)) ||
		(unit == 0 && shader == 0x2c7cb17dda0fab6cull);
}

constexpr bool IsIconQuad(uint64_t shader, uint32_t buffer, uint32_t stride, uint32_t first, uint32_t last)
{
	return shader == 0xb2877bf2bb2c2378ull && buffer == 0 && stride == 24 && first == 0 && last == 3;
}

constexpr bool IsMenuCompositePrimitive(uint64_t shader, uint32_t buffer, uint32_t stride, uint32_t first, uint32_t last)
{
	return buffer == 0 && stride == 24 && first == 0 &&
		((shader == 0xb13acb0b850e89a0ull && last == 3) ||
		 (shader == 0xee7e5e758b8c6999ull && last == 2));
}
}
