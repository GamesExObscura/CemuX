#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace PrimePublishedVideo
{
    inline bool IsTarget(uint64_t title, uint32_t version, uint32_t rpx)
    {
        if (title == 0x000500001010dd00ull)
            return (version == 0 && rpx == 0xe426355b) ||
                (version == 32 && rpx == 0x577202f4);
        return version == 0 &&
            ((title == 0x000500001010b500ull && rpx == 0x93f33377) ||
             (title == 0x000500001014c100ull && rpx == 0xfab814f1) ||
             (title == 0x0005000010145500ull && rpx == 0x793f26e5));
    }

    inline bool IsPlane(uint32_t slot, uint32_t width, uint32_t height, uint32_t pitch)
    {
        return (slot == 0 && width == 1280 && height == 720 && pitch == 1280) ||
            (slot > 0 && slot < 3 && width == 640 && height == 360 && pitch == 768);
    }

    struct Frame
    {
        std::array<uint32_t, 3> addresses{};
        std::array<std::shared_ptr<const std::vector<uint8_t>>, 3> planes{};
    };
    struct Bank
    {
        Frame pending;
        uint32_t mask = 0;
        std::shared_ptr<const Frame> published;
    };
    inline std::array<Bank, 4> banks{};
    inline std::mutex mutex;
    inline uint32_t bindingBank = 0;
    inline uint32_t nextBank = 0;
    inline bool submittedFramePending = false;

    inline bool ConsumeSubmittedFrame()
    {
        std::scoped_lock lock(mutex);
        const bool pending = submittedFramePending;
        submittedFramePending = false;
        return pending;
    }

    inline void Reset()
    {
        std::scoped_lock lock(mutex);
        banks = {};
        bindingBank = nextBank = 0;
        submittedFramePending = false;
    }

    inline void Submit(uint32_t slot, uint32_t address, const uint8_t* data,
        uint32_t width, uint32_t height, uint32_t pitch)
    {
        if (!address || !data || !IsPlane(slot, width, height, pitch))
            return;
        auto bytes = std::make_shared<std::vector<uint8_t>>(width * height);
        // Chroma rows include 128 bytes of padding; the host upload is tightly packed.
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(bytes->data() + row * width, data + row * pitch, width);
        std::scoped_lock lock(mutex);
        if (slot == 0)
        {
            uint32_t found = 0;
            for (; found < banks.size(); ++found)
                if (banks[found].pending.addresses[0] == address)
                    break;
            if (found == banks.size())
            {
                found = nextBank++ % banks.size();
                banks[found] = {};
            }
            bindingBank = found;
            banks[found].mask = 0;
        }
        auto& bank = banks[bindingBank];
        if (slot && !(bank.mask & 1))
            return;
        bank.pending.addresses[slot] = address;
        bank.pending.planes[slot] = std::move(bytes);
        bank.mask |= 1u << slot;
        if (bank.mask == 7)
        {
            bank.published = std::make_shared<Frame>(bank.pending);
            submittedFramePending = true;
            bank.mask = 0;
        }
    }

    inline std::shared_ptr<const Frame> Acquire(const std::array<uint32_t, 3>& addresses)
    {
        std::scoped_lock lock(mutex);
        for (const auto& bank : banks)
            if (bank.published && bank.published->addresses == addresses)
                return bank.published;
        return nullptr;
    }
}
