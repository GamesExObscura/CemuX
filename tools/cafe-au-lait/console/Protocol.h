#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include "vendor/sha256.h"
}
#include "cal_policy.h"

namespace cal {
constexpr uint32_t MaxFiles = 20000;
constexpr uint64_t MaxFile = 256ULL * 1024 * 1024;
constexpr uint64_t MaxTotal = 2ULL * 1024 * 1024 * 1024;
constexpr size_t Chunk = 32768;
constexpr uint16_t Port = 27581;

inline bool safeRelative(const std::string& path) {
    if (path.empty() || path.size() > 480 || path.front() == '/' || path.back() == '/') return false;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size()) {
            const auto c = static_cast<unsigned char>(path[i]);
            if (c < 32 || c > 126 || std::strchr("\\:<>\"|?*", c)) return false;
            if (c != '/') continue;
        }
        const auto part = path.substr(start, i - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        start = i + 1;
    }
    return true;
}
inline bool under(const std::string& path, const std::string& root) {
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/';
}
inline bool accountValid(const std::string& account) {
    if (account.size() != 8 || account[0] != '8' || account == "80000000") return false;
    return std::all_of(account.begin(), account.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
inline bool allowed(const std::string& path, const std::string& account) {
    if (!safeRelative(path) || !accountValid(account)) return false;
    if (path == "otp.bin" || path == "seeprom.bin") return true;
    for (const auto* name : minimalNativeLibraries) if (path == "cafeLibs/" + std::string(name)) return true;
    for (const auto* id : minimalSystemApps) {
        if (under(path, "mlc01/sys/title/00050010/" + std::string(id)) || under(path, "mlc01/usr/save/00050010/" + std::string(id))) return true;
    }
    for (const auto* id : minimalApplets) {
        if (under(path, "mlc01/sys/title/00050030/" + std::string(id)) || under(path, "mlc01/usr/save/00050030/" + std::string(id))) return true;
    }
    for (const auto* id : minimalShared) if (under(path, "mlc01/sys/title/0005001b/" + std::string(id))) return true;
    for (const auto* p : minimalSystemState) if (under(path, "mlc01/" + std::string(p))) return true;
    return under(path, "mlc01/usr/save/system/act/" + account);
}
inline bool privateIpv4(uint32_t host) {
    return (host >> 24) == 10 || (host >> 20) == 0xac1 || (host >> 16) == 0xc0a8 || (host >> 16) == 0xa9fe;
}
inline bool sameLan(uint32_t local, uint32_t peer, uint32_t mask) {
    if (!privateIpv4(local) || !privateIpv4(peer) || !mask || local == peer) return false;
    const auto inv = ~mask;
    if ((inv & (inv + 1)) != 0 || inv < 3 || mask < 0xff000000) return false;
    return (local & mask) == (peer & mask) && (peer & inv) != 0 && (peer & inv) != inv;
}
struct Entry { std::string path; uint64_t size; bool directory = false; };
using Send = std::function<void(const void*, size_t)>;
using Read = std::function<size_t(const Entry&, uint64_t, void*, size_t)>;
inline void number(const Send& send, uint64_t n, size_t count) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < count; ++i) bytes[count - i - 1] = static_cast<uint8_t>(n >> (8 * i));
    send(bytes.data(), count);
}
inline void transfer(const std::string& account, const std::vector<Entry>& files, const Send& send, const Read& read, uint8_t* buffer) {
    if (!accountValid(account) || files.empty() || files.size() > MaxFiles || !buffer) throw std::runtime_error("Invalid snapshot limits");
    uint64_t total = 0;
    std::vector<std::string> seen;
    seen.reserve(files.size());
    for (const auto& file : files) {
        if (!allowed(file.path, account) || file.size > MaxFile || total > MaxTotal - file.size) throw std::runtime_error("Snapshot outside approved scope");
        if ((file.path == "otp.bin" && file.size != 1024) || (file.path == "seeprom.bin" && file.size != 512)) throw std::runtime_error("Invalid console identity size");
        total += file.size;
        auto name = file.path;
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        seen.push_back(std::move(name));
    }
    std::sort(seen.begin(), seen.end());
    if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) throw std::runtime_error("Duplicate snapshot path");
    send("CALDUMP1", 8); send(account.data(), 8); number(send, files.size(), 4);
    for (const auto& file : files) {
        number(send, file.path.size(), 4); number(send, file.size, 8); send(file.path.data(), file.path.size());
        SHA256_CTX hash{}; sha256_init(&hash);
        for (uint64_t offset = 0; offset < file.size;) {
            const auto need = static_cast<size_t>(std::min<uint64_t>(Chunk, file.size - offset));
            const auto got = read(file, offset, buffer, need);
            if (got != need) throw std::runtime_error("Source changed or read failed; snapshot incomplete");
            sha256_update(&hash, buffer, got); send(buffer, got); offset += got;
        }
        std::array<uint8_t, 32> digest{}; sha256_final(&hash, digest.data()); send(digest.data(), digest.size());
    }
    send("CALDONE1", 8);
}
}
