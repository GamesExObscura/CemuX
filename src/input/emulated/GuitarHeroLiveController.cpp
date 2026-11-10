#include "input/emulated/GuitarHeroLiveController.h"
#include "input/api/Controller.h"

void GuitarHeroLiveController::load(const pugi::xml_node& node)
{
	SetMotionTilt(node.child("guitar").child("motion_tilt").text().as_bool(false));
}

void GuitarHeroLiveController::save(pugi::xml_node& node)
{
	node.append_child("guitar").append_child("motion_tilt").text().set(GetMotionTilt());
}

void GuitarHeroLiveController::SetMotionTilt(bool enabled)
{
	std::unique_lock lock(m_mutex);
	m_motionTilt = enabled;
	m_mapper.motion.Reset();
}

bool GuitarHeroLiveController::GetMotionTilt() const
{
	std::shared_lock lock(m_mutex);
	return m_motionTilt;
}

void GuitarHeroLiveController::RecenterMotion()
{
	std::unique_lock lock(m_mutex);
	m_mapper.motion.Reset();
}

bool GuitarHeroLiveController::MotionReady() const
{
	std::shared_lock lock(m_mutex);
	return m_mapper.motion.Ready();
}

GuitarHeroLive::State GuitarHeroLiveController::ReadState()
{
	std::unique_lock lock(m_mutex);
	std::array<float, GuitarHeroLive::Count> values{};
	std::array<float, 3> acc{};
	std::shared_ptr<ControllerBase> motionSource;
	for (const auto& controller : m_controllers)
	{
		controller->update_state();
		if (m_motionTilt && !motionSource && controller->is_connected() && controller->use_motion())
		{
			motionSource = controller;
			controller->get_motion_sample().getAccelerometer(acc.data());
		}
	}
	for (const auto& [mapping, source] : m_mappings)
	{
		if (mapping >= values.size()) continue;
		if (auto controller = source.controller.lock(); controller && controller->is_connected())
			values[mapping] = controller->get_axis_value(source.button);
	}
	const auto now = std::chrono::steady_clock::now();
	float dt = std::chrono::duration<float>(now - m_lastSample).count();
	if (m_motionSource.lock() != motionSource)
		m_mapper = {};
	// Keep the user's neutral pose when moving from Input settings into the game.
	if (dt > 0.25f)
		dt = 0.001f;
	m_motionSource = motionSource;
	m_lastSample = now;
	return m_mapper.Update(values, motionSource != nullptr, acc, dt);
}
