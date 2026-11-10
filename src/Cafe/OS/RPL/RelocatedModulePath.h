#pragma once
#include <string_view>

namespace RelocatedModulePath
{
constexpr std::string_view GetTitleCodeFilename(std::string_view path)
{
    if (path.empty() || path.front() != '/')
        return {};
    const auto name = path.substr(path.find_last_of('/') + 1);
    if (name.size() <= 4 || name.find_first_of("\\:") != std::string_view::npos)
        return {};
    const auto suffix = name.substr(name.size() - 4);
    const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
    if (suffix[0] != '.' || lower(suffix[1]) != 'r' || lower(suffix[2]) != 'p' || lower(suffix[3]) != 'l')
        return {};
    return name;
}
}
