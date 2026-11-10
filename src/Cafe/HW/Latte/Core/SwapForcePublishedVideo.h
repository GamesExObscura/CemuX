#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace SwapForcePublishedVideo
{
    inline bool IsTarget(uint64_t title, uint32_t version, uint32_t rpx)
    {
        return title == 0x0005000010139200ull &&
            ((version == 0 && rpx == 0x6cd668ab) ||
             (version == 16 && rpx == 0x43d160e2));
    }

    using Bytes = std::vector<uint8_t>;
    struct Frame
    {
        std::array<uint32_t, 3> addresses{};
        std::array<std::shared_ptr<const Bytes>, 3> planes{};
    };

    inline std::mutex mutex;
    inline constexpr std::array<uint32_t, 3> sizes{2048 * 1024, 1024 * 512, 1024 * 512};
    struct Bank
    {
        std::array<uint32_t, 3> addresses{};
        std::array<std::shared_ptr<const Bytes>, 3> pending{};
        uint32_t registeredMask = 0;
        uint32_t pendingMask = 0;
        std::shared_ptr<const Frame> published;
    };
    inline std::array<Bank, 4> banks{};
    inline uint32_t bindingBank = 0;
    inline uint32_t nextBank = 0;

    inline void Reset()
    {
        std::scoped_lock lock(mutex);
        banks = {};
        bindingBank = nextBank = 0;
    }

    inline void Register(uint32_t slot, uint32_t address)
    {
        if (slot >= 3 || address == 0)
            return;
        std::scoped_lock lock(mutex);
        if (slot == 0)
        {
            uint32_t found = 0;
            for (; found < banks.size(); ++found)
                if (banks[found].addresses[0] == address)
                    break;
            if (found == banks.size())
            {
                found = nextBank++ % banks.size();
                banks[found] = {};
            }
            bindingBank = found;
        }
        auto& bank = banks[bindingBank];
        if (bank.addresses[slot] != address)
        {
            bank.addresses[slot] = address;
            bank.pending = {};
            bank.pendingMask = 0;
            bank.published.reset();
        }
        bank.registeredMask |= 1u << slot;
    }

    inline void PublishPlane(uint32_t address, const void* data, uint32_t size)
    {
        std::scoped_lock lock(mutex);
        for (auto& bank : banks)
        {
            if (bank.registeredMask != 7)
                continue;
            for (uint32_t slot = 0; slot < 3; ++slot)
            {
                if (bank.addresses[slot] != address || sizes[slot] != size)
                    continue;
                // Preserve the CPU writeback before Bink reuses either working buffer.
                auto bytes = std::make_shared<Bytes>(size);
                std::memcpy(bytes->data(), data, size);
                bank.pending[slot] = std::move(bytes);
                bank.pendingMask |= 1u << slot;
                if (bank.pendingMask == 7)
                {
                    bank.published = std::make_shared<Frame>(Frame{bank.addresses, bank.pending});
                    bank.pending = {};
                    bank.pendingMask = 0;
                }
                return;
            }
        }
    }

    inline std::shared_ptr<const Frame> Acquire(uint32_t address)
    {
        std::scoped_lock lock(mutex);
        for (const auto& bank : banks)
            if (bank.addresses[0] == address)
                return bank.published;
        return nullptr;
    }
}
