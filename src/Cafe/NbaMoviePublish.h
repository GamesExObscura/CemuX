#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace NbaMoviePublish
{
constexpr uint32_t SelectConversion = 0x02fc55a4;
constexpr uint32_t FullConversion = 0x02fc564c;
constexpr uint32_t ParallelConversion = 0x02fc55a8;
constexpr uint32_t Width = 0x183c, Height = 0x1840;
constexpr uint32_t OutputMode = 0x1838, Parallel = 0x1848;

constexpr bool IsTarget(uint64_t title, uint32_t version, uint32_t hash,
    std::string_view module, uint32_t crc)
{
    return title == 0x000500001010da00ull && version == 32 && hash == 0xef62d66e &&
        module == "nba_clean_opt" && crc == 0xc58efab6;
}

constexpr bool UseFullFrame(uint32_t width, uint32_t height, uint32_t mode, uint32_t parallel)
{
    return width == 1024 && height == 576 && mode == 0 && parallel == 1;
}

inline constexpr std::array<std::array<uint32_t, 2>, 18> Instructions{{
    {SelectConversion, 0x418200a8}, {0x02fc5574, 0x9421ffe0},
    {0x02fc5590, 0x817d1838}, {0x02fc559c, 0x80dd1848},
    {0x02fc55a0, 0x2c060000}, {0x02fc55f0, 0x4bf5daad},
    {0x02fc5674, 0x48001f05}, {0x02fc7578, 0x80c30008},
    {0x02fc7580, 0x4bfffe64}, {0x02fc58a4, 0x4bfff85d},
    {0x02fc53d4, 0x815e1848}, {0x02fc73e4, 0x9421ffe0},
    {0x02fc744c, 0x101c410c}, {0x02fc7474, 0x10e96420},
    {0x02fc7480, 0xf0fe2000}, {0x02fc7514, 0xf0be201e},
    {0x02fc7554, 0x7d2b5838}, {0x02fc7574, 0x4e800020}
}};
}
