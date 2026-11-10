#pragma once
#include "Protocol.h"

namespace cal {
constexpr uint32_t FullMaxEntries = 250000;
constexpr uint64_t FullMaxFile = 0xffffffffULL;
constexpr uint64_t FullMaxTotal = 1024ULL * 1024 * 1024 * 1024;
constexpr uint64_t FullMaxMetadata = 64ULL * 1024 * 1024;

inline bool fullSafeRelative(const std::string& path) {
    if (path.empty() || path.size() > 480 || path.front() == '/' || path.back() == '/') return false;
    for (size_t i = 0; i < path.size();) {
        const auto c = static_cast<uint8_t>(path[i++]);
        if (c < 0x80) { if (c < 32 || c == 127 || std::strchr("\\:<>\"|?*", c)) return false; continue; }
        const unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!n || i + n > path.size()) return false;
        const auto next = static_cast<uint8_t>(path[i]);
        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) || (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) return false;
        for (unsigned j = 0; j < n; ++j) if ((static_cast<uint8_t>(path[i++]) & 0xc0) != 0x80) return false;
    }
    size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        auto device = part.substr(0, part.find('.'));
        for (auto& c : device) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" ||
            (device.size() == 4 && (device.compare(0, 3, "COM") == 0 || device.compare(0, 3, "LPT") == 0) && device[3] >= '1' && device[3] <= '9')) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
inline bool fullAllowed(const std::string& path, bool directory) {
    if (!fullSafeRelative(path)) return false;
    if (path == "mlc01") return directory;
    if (under(path, "mlc01")) return true;
    if (directory) return false;
    if (path == "otp.bin" || path == "seeprom.bin") return true;
    for (const auto* name : minimalNativeLibraries) if (path == "cafeLibs/" + std::string(name)) return true;
    return false;
}
inline void fullTransfer(const std::string& account, const std::vector<Entry>& files, const Send& send, const Read& read, uint8_t* buffer) {
    if (!accountValid(account) || files.empty() || files.size() > FullMaxEntries || !buffer) throw std::runtime_error("Invalid full MLC inventory");
    uint64_t total = 0, metadata = 0;
    bool root = false, otp = false, seeprom = false, selectedAccount = false;
    unsigned nativeCount = 0;
    std::vector<std::string> seen;
    seen.reserve(files.size());
    for (const auto& entry : files) {
        if (!fullAllowed(entry.path, entry.directory) || entry.size > FullMaxFile || (entry.directory && entry.size) || total > FullMaxTotal - entry.size) throw std::runtime_error("Full MLC inventory outside supported bounds; nothing omitted");
        if ((entry.path == "otp.bin" && entry.size != 1024) || (entry.path == "seeprom.bin" && entry.size != 512)) throw std::runtime_error("Invalid console identity size");
        total += entry.size; metadata += entry.path.size() + 64;
        if (metadata > FullMaxMetadata) throw std::runtime_error("Full MLC metadata limit exceeded; no partial export");
        root |= entry.path == "mlc01" && entry.directory;
        if (!entry.directory) {
            otp |= entry.path == "otp.bin";
            seeprom |= entry.path == "seeprom.bin";
            selectedAccount |= entry.path == "mlc01/usr/save/system/act/" + account + "/account.dat";
            for (const auto* library : minimalNativeLibraries) nativeCount += entry.path == "cafeLibs/" + std::string(library);
        }
        auto name = entry.path;
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        seen.push_back(std::move(name));
    }
    std::sort(seen.begin(), seen.end());
    if (!root || !otp || !seeprom || !selectedAccount || nativeCount != std::size(minimalNativeLibraries) || std::adjacent_find(seen.begin(), seen.end()) != seen.end()) throw std::runtime_error("Missing full MLC/online requirements or duplicate entry");
    SHA256_CTX inventory{}; sha256_init(&inventory);
    const Send header = [&](const void* p, size_t n) { sha256_update(&inventory, static_cast<const uint8_t*>(p), n); send(p, n); };
    header("CALDUMP2", 8); header(account.data(), 8); number(header, files.size(), 4); number(header, total, 8);
    for (const auto& file : files) {
        number(header, file.directory ? 1 : 0, 1); number(header, file.path.size(), 4); number(header, file.size, 8); header(file.path.data(), file.path.size());
        SHA256_CTX hash{}; sha256_init(&hash);
        for (uint64_t offset = 0; offset < file.size;) {
            const auto need = static_cast<size_t>(std::min<uint64_t>(Chunk, file.size - offset));
            const auto got = read(file, offset, buffer, need);
            if (got != need) throw std::runtime_error("Source changed/read failed; full MLC export incomplete");
            sha256_update(&hash, buffer, got); send(buffer, got); offset += got;
        }
        std::array<uint8_t, 32> digest{}; sha256_final(&hash, digest.data()); header(digest.data(), digest.size());
    }
    std::array<uint8_t, 32> digest{}; sha256_final(&inventory, digest.data());
    send("CALDONE2", 8); send(digest.data(), digest.size());
}
}
