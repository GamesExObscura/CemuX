#pragma once
#include <array>
#include <cstdint>
#include <mutex>

namespace SpiderDirectScanout
{
constexpr bool Enabled(uint64_t title, uint32_t version, uint32_t rpx)
{
    return title == 0x000500001011c300ull && version == 0 && rpx == 0x8e00935b;
}

struct Buffer { uint32_t address{}, size{}, format{}; };
struct Registry
{
    std::array<Buffer, 2> buffers{};
    void Configure(unsigned index, uint32_t address, uint32_t size, uint32_t format, uint32_t mode)
    {
        if (index >= buffers.size()) return;
        buffers[index] = {};
        if (mode == 1 && address && size && uint64_t(address) + size <= 0x100000000ull &&
            (format == 0x1a || format == 0x41a))
            buffers[index] = {address, size, format};
    }
    uint32_t Match(uint32_t address, uint32_t size, uint32_t format) const
    {
        for (unsigned i = 0; i < buffers.size(); ++i)
        {
            const auto& b = buffers[i];
            if (address && address == b.address && size && size <= b.size && format == b.format)
                return i == 0 ? 1 : 4;
        }
        return 0;
    }
};
inline Registry registry;
inline std::mutex mutex;
}
