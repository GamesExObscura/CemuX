#include "FullMlcProtocol.h"
#include "NativeLibraryAccess.h"
#include <coreinit/filesystem_fsa.h>
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <mocha/mocha.h>
#include <mocha/otp.h>
#include <nn/ac.h>
#include <whb/proc.h>
#include <coreinit/title.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <cerrno>
#include <memory>
#include <set>
#include <map>
#include <nn/act.h>
#include "Display.h"

namespace {
bool appRunning = true;
bool running() {
    if (!appRunning) return false;
    appRunning = WHBProcIsRunning();
    return appRunning;
}
void errorText(const char* message) {
    std::array<wchar_t, 256> text{};
    for (size_t i = 0; i + 1 < text.size() && message[i]; ++i) text[i] = static_cast<unsigned char>(message[i]);
    WHBLogFreetypePrint(text.data()); WHBLogFreetypeDrawScreen();
}
struct Socket {
    int fd = -1;
    Socket() = default;
    explicit Socket(int f): fd(f) {}
    Socket(const Socket&) = delete;
    ~Socket() { if (fd >= 0) close(fd); }
};
uint32_t pressed() {
    VPADStatus pad{}; VPADReadError error{};
    return VPADRead(VPAD_CHAN_0, &pad, 1, &error) > 0 && error == VPAD_READ_SUCCESS ? pad.trigger : 0;
}
void tick(bool cancel = true) {
    if (!running() || (cancel && (pressed() & VPAD_BUTTON_B))) throw std::runtime_error("Cancelled safely");
}
void screen(const wchar_t* title, const wchar_t* line = L"") {
    WHBLogFreetypeStartScreen(); WHBLogFreetypePrint(L"Cafe au Lait - PRIVATE");
    WHBLogFreetypePrint(title); WHBLogFreetypePrint(line); WHBLogFreetypeDrawScreen();
}
bool confirm(const wchar_t* title, const wchar_t* line = L"") {
    screen(title, line);
    while (running()) {
        const auto p = pressed();
        if (p & VPAD_BUTTON_B) return false;
        if (p & VPAD_BUTTON_A) return true;
        OSSleepTicks(OSMillisecondsToTicks(30));
    }
    return false;
}
void ready(int fd, bool writing, int seconds = 15) {
    const auto deadline = OSGetTime() + OSSecondsToTicks(seconds);
    while (OSGetTime() < deadline) {
        tick(); fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        timeval timeout{0, 100000};
        const int result = select(fd + 1, writing ? nullptr : &fds, writing ? &fds : nullptr, nullptr, &timeout);
        if (result > 0) return;
        if (result < 0 && errno != EINTR) throw std::runtime_error("Network wait failed");
    }
    throw std::runtime_error("Network timeout; no console files changed");
}
void nonblocking(int fd) {
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one)) != 0) throw std::runtime_error("Cannot configure bounded network IO");
}
void socketWrite(int fd, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    auto deadline = OSGetTime() + OSSecondsToTicks(15);
    while (size) {
        if (OSGetTime() > deadline) throw std::runtime_error("Network send stalled");
        ready(fd, true);
        const auto n = send(fd, p, std::min<size_t>(size, cal::Chunk), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) { OSSleepTicks(OSMillisecondsToTicks(1)); continue; }
        if (n <= 0) throw std::runtime_error("PC disconnected; no console files changed");
        p += n; size -= n; deadline = OSGetTime() + OSSecondsToTicks(15);
    }
}
void socketRead(int fd, void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    auto deadline = OSGetTime() + OSSecondsToTicks(15);
    while (size) {
        if (OSGetTime() > deadline) throw std::runtime_error("Network request stalled");
        ready(fd, false);
        const auto n = recv(fd, p, size, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) { OSSleepTicks(OSMillisecondsToTicks(1)); continue; }
        if (n <= 0) throw std::runtime_error("PC disconnected");
        p += n; size -= n; deadline = OSGetTime() + OSSecondsToTicks(15);
    }
}
struct NativeFsaOperations {
    int& client;
    int mount(const char* device, const char* target) {
        // Match libmocha/FTPiiU: SLC uses a GLOBAL filesystem mount.
        // The target remains private to this tool and cleanup is owned-only.
        return FSAMount(client, device, target, FSA_MOUNT_FLAG_GLOBAL_MOUNT, nullptr, 0);
    }
    int unmount(const char* target) noexcept {
        const auto status = FSAUnmount(client, target, FSA_UNMOUNT_FLAG_NONE);
        if (status < 0) OSReport("Cafe au Lait: owned SLC unmount failed: FSA=%d\n", status);
        return status;
    }
    cal::NativeProbe probe(const std::string& path) {
        FSAFileHandle handle{};
        const auto status = FSAOpenFileEx(client, path.c_str(), "r", static_cast<FSMode>(0), FS_OPEN_FLAG_NONE, 0, &handle);
        if (status < 0) return {status};
        struct Close { int client; FSAFileHandle handle; ~Close() { FSACloseFile(client, handle); } } closeFile{client, handle};
        alignas(64) FSAStat stat{};
        alignas(64) std::array<uint8_t, 64> header{};
        const auto statResult = FSAGetStatFile(client, handle, &stat);
        if (statResult < 0)
            throw std::runtime_error("Native library stat failed: " + path + "; FSA=" + std::to_string(statResult));
        if (stat.size < 20 || stat.size > cal::MaxFile || (stat.flags & (FS_STAT_DIRECTORY | FS_STAT_LINK | FS_STAT_ENCRYPTED_FILE)))
            throw std::runtime_error("Invalid native library type/size: " + path);
        const auto readResult = FSAReadFileWithPos(client, header.data(), 1, 20, 0, handle, 0);
        if (readResult != 20)
            throw std::runtime_error("Native library header read failed: " + path + "; FSA=" + std::to_string(readResult));
        if (std::memcmp(header.data(), "\x7f" "ELF\x01\x02", 6) || header[18] != 0 || header[19] != 20)
            throw std::runtime_error("Invalid native PowerPC library header: " + path);
        return {0, stat.size};
    }
};
struct ReadOnlyFiles {
    int client = -1;
    FSAFileHandle file = 0;
    bool fileOpen = false;
    std::string current;
    NativeFsaOperations nativeOperations{client};
    cal::NativeLibraryAccess<NativeFsaOperations> nativeAccess{nativeOperations};
    ReadOnlyFiles() {
        client = FSAAddClient(nullptr);
        if (client < 0) throw std::runtime_error("Cannot open filesystem client");
        if (Mocha_UnlockFSClientEx(client) != MOCHA_RESULT_SUCCESS) { FSADelClient(client); client = -1; throw std::runtime_error("System-file read access unavailable"); }
    }
    ~ReadOnlyFiles() {
        if (fileOpen) FSACloseFile(client, file);
        // Release file handles before our owned mount, and the mount before its client.
        nativeAccess.close();
        if (client >= 0) FSADelClient(client);
    }
    std::vector<cal::Entry> entries;
    std::map<std::string, std::string> nativeSources;
    uint64_t total = 0;
    uint64_t metadata = 0;
    unsigned directoriesVisited = 0;
    std::function<void()> progress;
    void add(cal::Entry entry) {
        metadata += entry.path.size() + 64;
        if (entries.size() >= cal::FullMaxEntries - 8 || metadata > cal::FullMaxMetadata || entry.size > cal::FullMaxFile || total > cal::FullMaxTotal - entry.size) throw std::runtime_error("Full MLC limits exceeded; no entries skipped");
        total += entry.size; entries.push_back(std::move(entry));
        if (progress) progress();
    }
    void walk(const std::string& relative) {
        struct Directory { int client; FSADirectoryHandle handle; ~Directory() { FSACloseDir(client, handle); } };
        std::vector<std::pair<std::string, unsigned>> pending{{relative, 0}};
        while (!pending.empty()) {
            tick(); const auto item = std::move(pending.back()); pending.pop_back();
            const auto relativePath = item.first.empty() ? std::string("mlc01") : "mlc01/" + item.first;
            if (item.second > 64 || ++directoriesVisited > cal::FullMaxEntries || !cal::fullSafeRelative(relativePath)) throw std::runtime_error("Directory limits exceeded; no entries skipped");
            const auto path = item.first.empty() ? std::string("/vol/storage_mlc01") : "/vol/storage_mlc01/" + item.first;
            FSADirectoryHandle handle = 0; const auto opened = FSAOpenDir(client, path.c_str(), &handle);
            if (opened < 0) throw std::runtime_error("Cannot read an approved system directory");
            Directory dir{client, handle};
            add({relativePath, 0, true});
            while (true) {
                tick(); alignas(64) FSADirectoryEntry entry{};
                const auto status = FSAReadDir(client, handle, &entry);
                if (status == FS_ERROR_END_OF_DIR) break;
                if (status < 0) throw std::runtime_error("Directory read failed");
                const auto length = strnlen(entry.name, sizeof(entry.name));
                if (!length || length == sizeof(entry.name)) throw std::runtime_error("Invalid system filename");
                const std::string name(entry.name, length);
                if (name == "." || name == "..") continue;
                if (name.find('/') != std::string::npos || !cal::fullSafeRelative(name)) throw std::runtime_error("Unrepresentable filename; full dump stopped, not filtered");
                const auto child = item.first.empty() ? name : item.first + "/" + name;
                if (!cal::fullSafeRelative("mlc01/" + child)) throw std::runtime_error("System path exceeds snapshot limits");
                if (entry.info.flags & (FS_STAT_LINK | FS_STAT_ENCRYPTED_FILE)) throw std::runtime_error("Linked/encrypted system entry is not supported");
                if (entry.info.flags & FS_STAT_DIRECTORY) {
                    if (pending.size() >= cal::FullMaxEntries) throw std::runtime_error("Too many system directories");
                    pending.emplace_back(child, item.second + 1);
                }
                else {
                    add({"mlc01/" + child, entry.info.size});
                }
            }
        }
    }
    void addNativeLibrary(const std::string& name) {
        const auto logical = "cafeLibs/" + name;
        const auto source = nativeAccess.resolve(name, [] { tick(); });
        add({logical, source.size}); nativeSources.emplace(logical, source.path);
    }
    size_t read(const cal::Entry& entry, uint64_t offset, void* buffer, size_t length) {
        if (entry.path != current) {
            if (fileOpen) { FSACloseFile(client, file); fileOpen = false; }
            const auto native = nativeSources.find(entry.path);
            if (native == nativeSources.end() && !entry.path.starts_with("mlc01/")) throw std::runtime_error("Unmapped approved file");
            const auto path = native != nativeSources.end() ? native->second : "/vol/storage_mlc01/" + entry.path.substr(6);
            if (FSAOpenFileEx(client, path.c_str(), "r", static_cast<FSMode>(0), FS_OPEN_FLAG_NONE, 0, &file) < 0) throw std::runtime_error("Cannot open approved file for reading");
            fileOpen = true;
            alignas(64) FSAStat stat{};
            if (FSAGetStatFile(client, file, &stat) < 0 || stat.size != entry.size) throw std::runtime_error("Source file changed during snapshot");
            current = entry.path;
        }
        const auto got = FSAReadFileWithPos(client, buffer, 1, length, static_cast<uint32_t>(offset), file, 0);
        return got < 0 ? 0 : static_cast<size_t>(got);
    }
};
void serve(const std::string& account) {
    uint32_t ip = 0, mask = 0;
    if (nn::ac::GetAssignedAddress(&ip).IsFailure() || nn::ac::GetAssignedSubnet(&mask).IsFailure() || !cal::privateIpv4(ntohl(ip))) throw std::runtime_error("Connect Wii U and PC to the same private home network first");
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.fd < 0) throw std::runtime_error("Cannot create network socket");
    nonblocking(listener.fd);
    int reuse = 1;
    if (setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) throw std::runtime_error("Cannot configure setup listener");
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = ip; local.sin_port = htons(cal::Port);
    if (bind(listener.fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || listen(listener.fd, 1) != 0) throw std::runtime_error("Setup port unavailable; close other setup instance");
    const auto host = ntohl(ip);
    WHBLogFreetypeStartScreen(); WHBLogFreetypePrint(L"Cafe au Lait");
    WHBLogFreetypePrintf(L"Wii U address: %u.%u.%u.%u", host >> 24, (host >> 16) & 255, (host >> 8) & 255, host & 255);
    WHBLogFreetypePrint(L"Open Cafe au Lait on the PC and enter this address.");
    WHBLogFreetypePrint(L"B: cancel. No console files will be written."); WHBLogFreetypeDrawScreen();
    ready(listener.fd, false, 180);
    sockaddr_in peer{}; socklen_t peerSize = sizeof(peer);
    Socket connection(accept(listener.fd, reinterpret_cast<sockaddr*>(&peer), &peerSize));
    if (connection.fd < 0 || peerSize != sizeof(peer) || !cal::sameLan(ntohl(ip), ntohl(peer.sin_addr.s_addr), ntohl(mask))) throw std::runtime_error("Only a PC on the same private subnet is allowed");
    nonblocking(connection.fd);
    std::array<uint8_t, 40> request{}; socketRead(connection.fd, request.data(), request.size());
    if (std::memcmp(request.data(), "CALPAIR1", 8) != 0) throw std::runtime_error("Not a Cafe au Lait client");
    std::array<wchar_t, 17> code{};
    for (size_t i = 0; i < 8; ++i) swprintf(code.data() + i * 2, 3, L"%02X", request[8 + i]);
    const auto peerHost = ntohl(peer.sin_addr.s_addr);
    WHBLogFreetypeStartScreen(); WHBLogFreetypePrint(L"Cafe au Lait - approve your PC");
    WHBLogFreetypePrintf(L"PC: %u.%u.%u.%u", peerHost >> 24, (peerHost >> 16) & 255, (peerHost >> 8) & 255, peerHost & 255);
    WHBLogFreetypePrintf(L"Compare the PC code: %S", code.data());
    WHBLogFreetypePrint(L"PRIVATE TEST: transfer is NOT encrypted (like FTP).");
    WHBLogFreetypePrint(L"Use only your trusted home LAN. Do not forward ports.");
    WHBLogFreetypePrint(L"A: approve FULL MLC and online-file copy. B: reject."); WHBLogFreetypeDrawScreen();
    const auto approvalDeadline = OSGetTime() + OSSecondsToTicks(90);
    while (true) {
        if (!running()) throw std::runtime_error("Application closing");
        const auto p = pressed();
        if (p & VPAD_BUTTON_B) throw std::runtime_error("Connection rejected");
        if (p & VPAD_BUTTON_A) break;
        if (OSGetTime() > approvalDeadline) throw std::runtime_error("Approval expired");
        OSSleepTicks(OSMillisecondsToTicks(30));
    }
    screen(L"Collecting FULL MLC and online files...", L"Includes internal games, saves and all accounts. Read-only.");
    ReadOnlyFiles files;
    auto nextProgress = OSGetTime();
    files.progress = [&] {
        if (OSGetTime() < nextProgress) return;
        socketWrite(connection.fd, "CALSCAN2", 8);
        cal::number([&](const void* p, size_t n) { socketWrite(connection.fd, p, n); }, files.entries.size(), 8);
        cal::number([&](const void* p, size_t n) { socketWrite(connection.fd, p, n); }, files.total, 8);
        nextProgress = OSGetTime() + OSSecondsToTicks(2);
    };
    for (const auto* name : minimalNativeLibraries) files.addNativeLibrary(name);
    files.walk("");
    alignas(64) WiiUConsoleOTP otp{}; alignas(64) std::array<uint8_t, 512> seeprom{};
    struct Wipe { uint8_t* p; size_t n; ~Wipe() { volatile uint8_t* v = p; while (n--) *v++ = 0; } } wipeOtp{reinterpret_cast<uint8_t*>(&otp), sizeof(otp)}, wipeSeeprom{seeprom.data(), seeprom.size()};
    // This libmocha implementation returns the SEEPROM byte count, unlike OTP.
    if (sizeof(otp) != 1024 || Mocha_ReadOTP(&otp) != MOCHA_RESULT_SUCCESS || static_cast<int>(Mocha_SEEPROMRead(seeprom.data(), 0, seeprom.size())) != static_cast<int>(seeprom.size())) throw std::runtime_error("Console identity read failed");
    files.entries.push_back({"otp.bin", sizeof(otp)}); files.entries.push_back({"seeprom.bin", seeprom.size()});
    std::unique_ptr<uint8_t, decltype(&free)> buffer(static_cast<uint8_t*>(memalign(64, cal::Chunk)), free);
    if (!buffer) throw std::bad_alloc();
    screen(L"Copying to your approved PC...", L"B: cancel. No data is written to the Wii U or SD card.");
    cal::fullTransfer(account, files.entries,
        [&](const void* p, size_t n) { socketWrite(connection.fd, p, n); },
        [&](const cal::Entry& entry, uint64_t offset, void* p, size_t n) {
            tick();
            if (entry.path == "otp.bin") { std::memcpy(p, reinterpret_cast<uint8_t*>(&otp) + offset, n); return n; }
            if (entry.path == "seeprom.bin") { std::memcpy(p, seeprom.data() + offset, n); return n; }
            return files.read(entry, offset, p, n);
        }, buffer.get());
    shutdown(connection.fd, SHUT_WR);
}
}
int main() {
    WHBProcInit();
    if (WHBLogFreetypeInit() != 0) { WHBProcShutdown(); return 1; }
    WHBLogFreetypeSetBackgroundColor(0x2b211b00); WHBLogFreetypeSetFontColor(0xffecd6ff);
    VPADInit(); FSAInit(); nn::act::Initialize(); nn::ac::Initialize();
    bool mocha = false;
    try {
        uint32_t version = 0;
        if (Mocha_CheckAPIVersion(&version) != MOCHA_RESULT_SUCCESS || Mocha_InitLibrary() != MOCHA_RESULT_SUCCESS) throw std::runtime_error("Compatible Aroma/Tiramisu/Mocha setup is required; no exploit will be installed");
        mocha = true;
        std::vector<std::pair<nn::act::SlotNo, std::string>> users;
        for (nn::act::SlotNo slot = 1; slot <= 12; ++slot) if (nn::act::IsSlotOccupied(slot)) {
            char id[9]{}; snprintf(id, sizeof(id), "%08x", nn::act::GetPersistentIdEx(slot));
            if (cal::accountValid(id)) users.emplace_back(slot, id);
        }
        if (users.empty()) throw std::runtime_error("No valid console account found");
        size_t selected = 0;
        for (size_t i = 0; i < users.size(); ++i) if (users[i].first == nn::act::GetSlotNo()) selected = i;
        while (running()) {
            char accountName[nn::act::AccountIdSize]{};
            nn::act::GetAccountIdEx(accountName, users[selected].first);
            accountName[nn::act::AccountIdSize - 1] = 0;
            const std::wstring displayName(accountName, accountName + strnlen(accountName, sizeof(accountName)));
            WHBLogFreetypeStartScreen(); WHBLogFreetypePrint(L"Cafe au Lait - PRIVATE trusted-LAN setup");
            WHBLogFreetypePrintf(L"Selected console account: %S", std::wstring(users[selected].second.begin(), users[selected].second.end()).c_str());
            WHBLogFreetypePrintf(L"Account name: %S", displayName.empty() ? L"(not linked)" : displayName.c_str());
            WHBLogFreetypePrintf(L"Saved password: %S", nn::act::IsPasswordCacheEnabledEx(users[selected].first) ? L"yes" : L"NO - enable before export");
            WHBLogFreetypePrint(L"Left/right: select account. A: wait for your PC. B: exit.");
            WHBLogFreetypePrint(L"FULL internal MLC: all files, empty folders, games, saves, accounts.");
            WHBLogFreetypePrint(L"Selected account should be your console-linked Pretendo PNID.");
            WHBLogFreetypePrint(L"No NAND/SEEPROM writes, install, formatting or raw USB/SD access.");
            WHBLogFreetypePrint(L"This private LAN test is not encrypted. Never expose it online."); WHBLogFreetypeDrawScreen();
            while (running()) {
                const auto p = pressed();
                if (p & VPAD_BUTTON_B) goto cleanup;
                if ((p & VPAD_BUTTON_LEFT) && selected) { --selected; break; }
                if ((p & VPAD_BUTTON_RIGHT) && selected + 1 < users.size()) { ++selected; break; }
                if (p & VPAD_BUTTON_A) {
                    try {
                        if (displayName.empty() || !nn::act::IsPasswordCacheEnabledEx(users[selected].first)) throw std::runtime_error("Select your linked PNID with Save password enabled on Wii U first");
                        serve(users[selected].second); if (!confirm(L"Copy finished. Validate/import on the PC.", L"A: return. B: exit.")) goto cleanup;
                    }
                    catch (const std::exception& e) {
                        screen(L"Transfer stopped safely. No console files were changed.");
                        errorText(e.what());
                        while (running()) { const auto key = pressed(); if (key & (VPAD_BUTTON_A | VPAD_BUTTON_B)) break; OSSleepTicks(OSMillisecondsToTicks(30)); }
                    }
                    break;
                }
                OSSleepTicks(OSMillisecondsToTicks(30));
            }
        }
    } catch (const std::exception& e) {
        screen(L"Cafe au Lait could not start. No console files were changed.");
        errorText(e.what());
        while (running()) { if (pressed() & VPAD_BUTTON_B) break; OSSleepTicks(OSMillisecondsToTicks(30)); }
    }
cleanup:
    if (mocha) Mocha_DeInitLibrary();
    nn::ac::Finalize(); nn::act::Finalize(); FSAShutdown();
    // Follow WUT's normal foreground-release path before releasing display memory.
    const auto title = OSGetTitleID();
    const bool hbl = title == 0x0005000013374842ULL || title == 0x000500101004a000ULL || title == 0x000500101004a100ULL || title == 0x000500101004a200ULL;
    if (appRunning) {
        if (hbl) WHBProcStopRunning();
        else SYSLaunchMenu();
        while (running()) OSSleepTicks(OSMillisecondsToTicks(30));
    }
    WHBLogFreetypeFree();
    VPADShutdown();
    WHBProcShutdown();
    return 0;
}
