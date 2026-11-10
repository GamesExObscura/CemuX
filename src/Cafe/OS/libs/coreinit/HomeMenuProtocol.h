#pragma once

#include <cstdint>

namespace coreinit::HomeMenuProtocol
{
	constexpr bool IsHomeMenuTitle(std::uint64_t titleId)
	{
		return titleId == 0x000500301001000aULL ||
			titleId == 0x000500301001010aULL ||
			titleId == 0x000500301001020aULL;
	}

	constexpr std::uint32_t ProcessId(std::uint64_t titleId)
	{
		return IsHomeMenuTitle(titleId) ? 5 : (titleId == 0x000500301001700aULL ||
			titleId == 0x000500301001710aULL || titleId == 0x000500301001720aULL ? 4 : 15);
	}

	constexpr bool IsManualTitle(std::uint64_t titleId) { return ProcessId(titleId) == 4; }

	class ActivationInput
	{
	public:
		bool Poll(std::uint32_t held, bool canActivate)
		{
			const auto pressed = held & ~m_held;
			m_held = held;
			if (!canActivate)
				m_pending = false;
			else if (pressed)
				m_pending = true;
			return m_pending;
		}

		void Acknowledge() { m_pending = false; }
		void Reset() { m_held = 0; m_pending = false; }

	private:
		std::uint32_t m_held{};
		bool m_pending{};
	};
}
