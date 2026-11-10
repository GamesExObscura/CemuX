#pragma once

#include "input/emulated/EmulatedController.h"
#include "input/emulated/GuitarHeroLiveState.h"
#include <chrono>

class GuitarHeroLiveController : public EmulatedController
{
public:
	explicit GuitarHeroLiveController(size_t player_index) : EmulatedController(player_index) {}
	Type type() const override { return Guitar; }
	void load(const pugi::xml_node& node) override;
	void save(pugi::xml_node& node) override;
	GuitarHeroLive::State ReadState();
	void SetMotionTilt(bool enabled);
	bool GetMotionTilt() const;
	void RecenterMotion();
	bool MotionReady() const;

	glm::vec2 get_axis() const override { return {}; }
	glm::vec2 get_rotation() const override { return {}; }
	glm::vec2 get_trigger() const override { return {}; }
	uint32 get_emulated_button_flag(uint32) const override { return 0; }
	size_t get_highest_mapping_id() const override { return GuitarHeroLive::Count; }
	bool is_axis_mapping(uint64 mapping) const override { return mapping == GuitarHeroLive::Whammy; }
	bool is_start_down() const override { return is_mapping_down(GuitarHeroLive::Start); }
	bool is_left_down() const override { return is_mapping_down(GuitarHeroLive::Left); }
	bool is_right_down() const override { return is_mapping_down(GuitarHeroLive::Right); }
	bool is_up_down() const override { return is_mapping_down(GuitarHeroLive::StrumUp); }
	bool is_down_down() const override { return is_mapping_down(GuitarHeroLive::StrumDown); }
	bool is_a_down() const override { return is_mapping_down(GuitarHeroLive::White1); }
	bool is_b_down() const override { return is_mapping_down(GuitarHeroLive::Black1); }
	bool is_home_down() const override { return false; }

private:
	bool m_motionTilt = false;
	GuitarHeroLive::Mapper m_mapper;
	std::weak_ptr<ControllerBase> m_motionSource;
	std::chrono::steady_clock::time_point m_lastSample{};
};
