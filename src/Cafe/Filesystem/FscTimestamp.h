#pragma once
#include <cstdint>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace FscTimestamp
{
    struct Times { std::uint64_t created{}, modified{}; };
    constexpr std::uint64_t FromWindows(std::uint64_t ticks)
    {
        constexpr std::uint64_t epoch = 119600064000000000ULL;
        return ticks >= epoch ? (ticks - epoch) / 10 : 0;
    }
    constexpr std::uint64_t FromUnix(std::int64_t seconds, std::int64_t nanoseconds)
    {
        constexpr std::int64_t epoch = 315532800;
        if (seconds < epoch || nanoseconds < 0 || nanoseconds >= 1000000000) return 0;
        const auto elapsed = static_cast<std::uint64_t>(seconds - epoch);
        if (elapsed > (UINT64_MAX - 999999) / 1000000) return 0;
        return elapsed * 1000000 + static_cast<std::uint64_t>(nanoseconds / 1000);
    }
    inline Times ReadPath(const std::filesystem::path& path)
    {
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return {};
        const auto ticks = [](const FILETIME& value) { return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        return {FromWindows(ticks(data.ftCreationTime)), FromWindows(ticks(data.ftLastWriteTime))};
#else
        struct stat data{};
        if (stat(path.c_str(), &data)) return {};
#if defined(__APPLE__)
        return {FromUnix(data.st_birthtimespec.tv_sec, data.st_birthtimespec.tv_nsec), FromUnix(data.st_mtimespec.tv_sec, data.st_mtimespec.tv_nsec)};
#elif defined(__FreeBSD__)
        return {FromUnix(data.st_birthtim.tv_sec, data.st_birthtim.tv_nsec), FromUnix(data.st_mtim.tv_sec, data.st_mtim.tv_nsec)};
#else
        // ctime is inode-change time, not creation time. Preserve unknown birth time.
        return {0, FromUnix(data.st_mtim.tv_sec, data.st_mtim.tv_nsec)};
#endif
#endif
    }
}
