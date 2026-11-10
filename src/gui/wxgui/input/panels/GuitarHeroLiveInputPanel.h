#pragma once
#include "wxgui/input/panels/InputPanel.h"
#include "input/emulated/GuitarHeroLiveController.h"

class wxCheckBox;
class wxStaticText;

class GuitarHeroLiveInputPanel : public InputPanel
{
public:
	explicit GuitarHeroLiveInputPanel(wxWindow* parent);
	void load_controller(const EmulatedControllerPtr& controller) override;
	void on_timer(const EmulatedControllerPtr& emulated, const ControllerPtr& controller) override;

private:
	std::weak_ptr<GuitarHeroLiveController> m_guitar;
	wxCheckBox* m_motion;
	wxStaticText* m_status;
};
