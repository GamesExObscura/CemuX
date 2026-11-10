#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZombiLayerOrder
{
constexpr char Fold(char c) { return c >= 'A' && c <= 'Z' ? char(c+('a'-'A')) : c; }

constexpr bool Enabled(std::uint64_t title, unsigned version, std::uint32_t rpx,
                       std::string_view path, std::size_t layers)
{
    if (title != 0x000500001010dd00ull || version != 32 || rpx != 0x577202f4 || layers < 2)
        return false;
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    constexpr std::string_view directory = "/vol/content/lynfsatest/BinCafe";
    if (path.size() != directory.size()) return false;
    for (std::size_t i=0; i<path.size(); ++i)
        if (Fold(path[i]) != Fold(directory[i])) return false;
    return true;
}

template<class Entries, class Entry, class Equal>
void Merge(Entries& entries, const Entry& entry, Equal equal)
{
    for (auto& previous : entries)
    {
        if (equal(previous,entry))
        {
            previous = entry;
            return;
        }
    }
    entries.emplace_back(entry);
}
}
