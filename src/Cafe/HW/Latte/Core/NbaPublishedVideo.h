#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace NbaPublishedVideo
{
    constexpr bool IsTarget(uint64_t title, uint32_t version, uint32_t rpx)
    {
        return title == 0x000500001010da00ull && version == 32 && rpx == 0xef62d66e;
    }

    constexpr bool IsLayout(uint32_t format, uint32_t tileMode, uint32_t dim,
        uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch,
        uint32_t levels, uint32_t aa, uint32_t firstMip, uint32_t mipCount,
        uint32_t firstSlice, uint32_t sliceCount)
    {
        return format == 0x28 && tileMode == 1 && dim == 1 &&
            width == 1024 && height == 576 && depth == 1 && pitch == 1024 &&
            levels == 1 && aa == 0 && firstMip == 0 && mipCount == 1 &&
            firstSlice == 0 && sliceCount == 1;
    }

    constexpr uint32_t PackedBytes = 1024 * 576 * 2;
    constexpr uint64_t MovieShader = 0x2b202f3672b41f12ull;

    struct Frame
    {
        uint32_t address;
        uint64_t sequence;
        std::vector<uint8_t> packed;
    };

    class Store
    {
    public:
        void Reset()
        {
            std::scoped_lock lock(m_mutex);
            m_frames = {};
            m_next = 0;
            m_sequence = 0;
        }

        bool Submit(uint32_t address, const uint8_t* data, uint32_t size)
        {
            if (!address || !data || size != PackedBytes)
                return false;
            auto frame = std::make_shared<Frame>();
            frame->address = address;
            frame->packed.assign(data, data + size);
            std::scoped_lock lock(m_mutex);
            frame->sequence = ++m_sequence;
            for (auto& existing : m_frames)
            {
                if (existing && existing->address == address)
                {
                    existing = std::move(frame);
                    return true;
                }
            }
            m_frames[m_next++ % m_frames.size()] = std::move(frame);
            return true;
        }

        std::shared_ptr<const Frame> Acquire(uint32_t address) const
        {
            std::scoped_lock lock(m_mutex);
            for (const auto& frame : m_frames)
                if (frame && frame->address == address)
                    return frame;
            return nullptr;
        }

    private:
        mutable std::mutex m_mutex;
        std::array<std::shared_ptr<const Frame>, 4> m_frames{};
        uint64_t m_sequence = 0;
        uint32_t m_next = 0;
    };

    inline Store frames;
}
