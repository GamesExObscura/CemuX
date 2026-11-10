#pragma once
#include "Protocol.h"

namespace cal {
inline constexpr const char* SlcDevice = "/dev/slc01";
inline constexpr const char* SlcPrivateMount = "/vol/storage_cal_slc01";
inline constexpr const char* SlcCodeDirectory = "/sys/title/00050010/1000400a/code/";

struct NativeProbe {
    int status;
    uint64_t size = 0;
};
struct NativeLibrary {
    std::string path;
    uint64_t size;
};

// Ops supplies read-only probing and a normal FSA filesystem mount, never raw IO.
// Mount ownership stays with this session, including when resolve/tick throws.
template<class Ops> class NativeLibraryAccess {
    Ops& ops;
    bool owned = false;
public:
    explicit NativeLibraryAccess(Ops& operations) : ops(operations) {}
    NativeLibraryAccess(const NativeLibraryAccess&) = delete;
    NativeLibraryAccess& operator=(const NativeLibraryAccess&) = delete;
    ~NativeLibraryAccess() { close(); }
    int close() noexcept {
        if (!owned) return 0;
        owned = false;
        return ops.unmount(SlcPrivateMount);
    }
    template<class Tick> NativeLibrary resolve(const std::string& name, Tick tick) {
        if (!allowed("cafeLibs/" + name, "80000001"))
            throw std::runtime_error("Unapproved native library");
        for (const auto* directory : {
                "/vol/system/title/00050010/1000400a/code/",
                "/vol/system_slc/sys/title/00050010/1000400a/code/",
                "/vol/storage_slc01/sys/title/00050010/1000400a/code/",
                "/vol/system/sys/title/00050010/1000400a/code/"}) {
            tick();
            const auto path = std::string(directory) + name;
            const auto found = ops.probe(path);
            if (found.status >= 0) return {path, found.size};
        }
        if (!owned) {
            tick();
            const int status = ops.mount(SlcDevice, SlcPrivateMount);
            // Do not adopt/unmount a conflicting mount, even if it already exists.
            if (status < 0)
                throw std::runtime_error("SLC mount for " + name + " failed: FSA=" + std::to_string(status));
            owned = true;
        }
        tick();
        const auto path = std::string(SlcPrivateMount) + SlcCodeDirectory + name;
        const auto found = ops.probe(path);
        if (found.status < 0)
            throw std::runtime_error("Native library " + name + " unavailable at " + path + "; FSA=" + std::to_string(found.status));
        return {path, found.size};
    }
};
}
