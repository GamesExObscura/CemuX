#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace GuitarHeroLive
{
enum Mapping : unsigned
{
	White1 = 1, White2, White3, Black1, Black2, Black3,
	StrumUp, StrumDown, Left, Right, Start, HeroPower, TV, Whammy, Tilt, Recenter, Count
};

struct State
{
	std::array<bool, 3> white{}, black{};
	bool strumUp{}, strumDown{}, left{}, right{}, start{}, heroPower{}, tv{};
	uint8_t whammy = 0x80, tilt = 0x80;
};

// Tilt uses the change in gravity direction, not gyro speed. Yaw alone is not tilt.
class TiltTracker
{
public:
	void Reset() { *this = {}; }
	bool Ready() const { return m_ready; }
	bool Update(std::array<float, 3> acc, float dt)
	{
		const float norm = std::sqrt(Dot(acc, acc));
		if (!std::isfinite(norm) || norm < 0.8f || norm > 1.2f || !std::isfinite(dt) || dt <= 0 || dt > 0.25f)
		{
			m_active = false;
			m_settled = 0;
			return false;
		}
		for (auto& v : acc) v /= norm;
		if (!m_ready)
		{
			if (m_settled == 0 || Dot(acc, m_neutral) < 0.996f)
			{
				m_neutral = acc;
				m_settled = 0;
			}
			m_settled += dt;
			m_filtered = acc;
			m_ready = m_settled >= 0.3f;
			return false;
		}
		const float alpha = 1.0f - std::exp(-dt / 0.08f);
		for (unsigned i = 0; i < 3; ++i) m_filtered[i] += alpha * (acc[i] - m_filtered[i]);
		const float len = std::sqrt(Dot(m_filtered, m_filtered));
		if (len < 0.01f) return m_active = false;
		const float cosine = Dot(m_filtered, m_neutral) / len;
		// Activate at 45 degrees; return below 30 degrees before re-arming.
		m_active = cosine <= (m_active ? 0.8660254f : 0.7071068f);
		return m_active;
	}

private:
	static float Dot(const std::array<float, 3>& a, const std::array<float, 3>& b)
	{
		return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
	}
	std::array<float, 3> m_neutral{}, m_filtered{};
	float m_settled = 0;
	bool m_ready = false, m_active = false;
};

class Mapper
{
public:
	State Update(const std::array<float, Count>& values, bool motionEnabled,
		const std::array<float, 3>& acc, float dt)
	{
		const bool wasRecenter = m_down[Recenter];
		for (unsigned i = 1; i < Count; ++i)
		{
			// Axis bindings need hysteresis; a held stick must not generate repeated strums.
			m_down[i] = std::isfinite(values[i]) && values[i] >= (m_down[i] ? 0.25f : 0.55f);
		}
		if (m_down[Recenter] && !wasRecenter) motion.Reset();
		bool tilted = false;
		if (motionEnabled) tilted = motion.Update(acc, dt);
		else motion.Reset();
		State result;
		result.white = {m_down[White1], m_down[White2], m_down[White3]};
		result.black = {m_down[Black1], m_down[Black2], m_down[Black3]};
		result.strumUp = m_down[StrumUp] && !m_down[StrumDown];
		result.strumDown = m_down[StrumDown] && !m_down[StrumUp];
		result.left = m_down[Left] && !m_down[Right];
		result.right = m_down[Right] && !m_down[Left];
		result.start = m_down[Start];
		result.heroPower = m_down[HeroPower];
		result.tv = m_down[TV];
		const float whammy = std::isfinite(values[Whammy]) ? std::clamp(values[Whammy], 0.0f, 1.0f) : 0;
		result.whammy = static_cast<uint8_t>(128.0f * (1.0f - whammy));
		result.tilt = (m_down[Tilt] || tilted) ? 0xff : 0x80;
		return result;
	}
	TiltTracker motion;
private:
	std::array<bool, Count> m_down{};
};
}
