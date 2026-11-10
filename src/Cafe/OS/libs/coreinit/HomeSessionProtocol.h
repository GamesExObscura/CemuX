#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include "HomeControllerBridge.h"

namespace coreinit::NativeHomeSession
{
	constexpr std::uint32_t SessionMagic = 0x48424d31;
	constexpr std::uint32_t SessionVersion = 9;
	constexpr bool CanActivatePreparedComponent(bool openingManual, std::int64_t elapsedMilliseconds)
	{
		return !openingManual || elapsedMilliseconds >= 2500;
	}
	struct SurfaceArea
	{
		std::uint64_t owner{};
		std::int32_t x{}, y{}, width{}, height{};
		bool Valid() const { return owner && width > 0 && height > 0; }
		bool operator==(const SurfaceArea&) const = default;
	};
	struct SurfaceLayout
	{
		SurfaceArea tv, pad;
		bool operator==(const SurfaceLayout&) const = default;
	};
	struct alignas(64) SharedSession
	{
		std::uint32_t magic = SessionMagic;
		std::uint32_t version = SessionVersion;
		std::uint32_t ownerPid{};
		std::uint32_t reserved{};
		std::uint64_t gameTitle{};
		std::uint64_t timerFrequency{};
		alignas(4) std::uint32_t activation{};
		std::uint32_t buttons{};
		std::uint32_t action{};
		std::uint32_t stop{};
		std::uint32_t ready{};
		std::uint32_t shown{};
		std::uint32_t resumeRequest{};
		std::uint32_t padRumble{};
		std::uint32_t padRumbleRevision{};
		std::uint32_t padBatteryLevel{0xc0};
		alignas(8) std::uint64_t activationTick{};
		std::uint32_t layoutSequence{};
		SurfaceLayout layout;
		HomeControllerBridge controllers;
	};
	static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
	inline std::uint32_t Load(std::uint32_t& value) { return std::atomic_ref(value).load(std::memory_order_acquire); }
	inline void Store(std::uint32_t& value, std::uint32_t data) { std::atomic_ref(value).store(data, std::memory_order_release); }

	inline void PublishLayout(SharedSession& session, const SurfaceLayout& layout)
	{
		const auto sequence = Load(session.layoutSequence);
		Store(session.layoutSequence, sequence + 1);
		auto write = [](SurfaceArea& to, const SurfaceArea& from) {
			std::atomic_ref(to.owner).store(from.owner);
			std::atomic_ref(to.x).store(from.x); std::atomic_ref(to.y).store(from.y);
			std::atomic_ref(to.width).store(from.width); std::atomic_ref(to.height).store(from.height);
		};
		write(session.layout.tv, layout.tv); write(session.layout.pad, layout.pad);
		Store(session.layoutSequence, sequence + 2);
	}

	inline bool ReadLayout(SharedSession& session, SurfaceLayout& layout, std::uint32_t& sequence)
	{
		sequence = Load(session.layoutSequence);
		if (!sequence || (sequence & 1)) return false;
		auto read = [](SurfaceArea& from) {
			return SurfaceArea{std::atomic_ref(from.owner).load(), std::atomic_ref(from.x).load(),
				std::atomic_ref(from.y).load(), std::atomic_ref(from.width).load(), std::atomic_ref(from.height).load()};
		};
		layout = {read(session.layout.tv), read(session.layout.pad)};
		return sequence == Load(session.layoutSequence);
	}

	constexpr bool IsGameTitle(std::uint64_t title)
	{
		return (title >> 32) == 0x00050000 || (title >> 32) == 0x00050002;
	}

	inline std::uint64_t ValidatedHelperTimerFrequency(SharedSession& session, std::uint32_t selfPid, bool ownerAlive)
	{
		if (session.magic != SessionMagic || session.version != SessionVersion || !ownerAlive ||
			!session.ownerPid || session.ownerPid == selfPid || !IsGameTitle(session.gameTitle) || Load(session.stop)) return 0;
		return session.timerFrequency >= 1000000 && session.timerFrequency <= 20000000000ULL ? session.timerFrequency : 0;
	}

	class ReleaseBarrier
	{
	public:
		void Begin() { m_cores.store(0); m_pending.store(true); }
		bool Notify(std::uint32_t core)
		{
			if (core > 2 || !m_pending.load()) return false;
			m_cores.fetch_or(1u << core);
			return true;
		}
		bool Complete() const { return m_pending.load() && m_cores.load() == 7; }
		void Reset() { m_pending.store(false); m_cores.store(0); }
	private:
		std::atomic<std::uint32_t> m_cores{};
		std::atomic<bool> m_pending{};
	};

	constexpr std::size_t HbmArgumentSize(std::uint32_t sdk) { return sdk >= 21103 ? 32 : 24; }

	// Normal HOME activation has no launch target or requested system-app switch.
	inline bool WriteNormalHbmArguments(std::span<std::uint8_t> output, std::uint32_t sdk)
	{
		const auto size = HbmArgumentSize(sdk);
		if (output.size() < size) return false;
		std::memset(output.data(), 0, size);
		std::memset(output.data() + 12, 0xff, 4);
		return true;
	}

	// The native getter's 24-byte ABI is independent of app.xml's minimum SDK.
	constexpr std::size_t ManualArgumentSize(std::uint32_t) { return 24; }
	inline bool WriteManualArguments(std::span<std::uint8_t> output, std::uint32_t sdk, std::uint64_t title)
	{
		const auto size = ManualArgumentSize(sdk);
		if (!title || output.size() < size) return false;
		std::memset(output.data(), 0, size);
		for (unsigned i = 0; i < 8; ++i) output[size - 8 + i] = static_cast<std::uint8_t>(title >> (56 - 8 * i));
		return true;
	}

	class ButtonHandoffGate
	{
	public:
		void Arm(std::uint32_t held) { m_blocked = held; }
		void Reset() { m_blocked = 0; }
		std::uint32_t Filter(std::uint32_t held)
		{
			m_blocked &= held;
			return held & ~m_blocked;
		}
	private:
		std::uint32_t m_blocked{};
	};

	struct ButtonEdges
	{
		std::atomic<std::uint32_t> previous{};
		void Reset() { previous.store(0); }
		void Read(std::uint32_t hold, std::uint32_t& pressed, std::uint32_t& released)
		{
			const auto old = previous.exchange(hold);
			pressed = hold & ~old;
			released = old & ~hold;
		}
	};
}
