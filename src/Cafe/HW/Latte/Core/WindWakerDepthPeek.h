#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace WindWakerDepthPeek
{
    constexpr bool IsTarget(uint64_t title, uint32_t version, uint32_t hash)
    {
        return title == 0x0005000010143500ULL && version == 0 && hash == 0xb1c033dd;
    }

    struct Point
    {
        int32_t x{};
        int32_t y{};
        uint32_t depth{};
    };

    inline uint32_t Pixel(int32_t coordinate, uint32_t extent, uint32_t guestExtent)
    {
        return static_cast<uint32_t>(std::clamp(static_cast<double>(coordinate) * extent / guestExtent,
            0.0, static_cast<double>(extent - 1)));
    }

    inline uint32_t Depth24(float depth)
    {
        return std::isfinite(depth) ? static_cast<uint32_t>(std::clamp(depth, 0.0f, 1.0f) * 16777215.0f) : 0;
    }

    bool ReadPixels(uint32_t physicalAddress, uint32_t width, uint32_t height, std::span<Point> points);
}
