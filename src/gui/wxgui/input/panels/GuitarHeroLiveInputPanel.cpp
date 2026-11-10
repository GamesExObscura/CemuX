#include "wxgui/input/panels/GuitarHeroLiveInputPanel.h"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/gbsizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

GuitarHeroLiveInputPanel::GuitarHeroLiveInputPanel(wxWindow* parent) : InputPanel(parent)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	auto* rows = new wxGridBagSizer(2, 6);
	const wxString labels[] = {
		_("White fret 1"), _("White fret 2"), _("White fret 3"),
		_("Black fret 1"), _("Black fret 2"), _("Black fret 3"),
		_("Strum up / Menu up"), _("Strum down / Menu down"),
		_("Menu left"), _("Menu right"), _("Start / Pause"),
		_("Hero Power"), _("GHTV"), _("Whammy"), _("Tilt (button / axis)"), _("Recenter motion")
	};
	for (unsigned id = 1; id < GuitarHeroLive::Count; ++id)
	{
		const int row = (id - 1) % 8, column = ((id - 1) / 8) * 2;
		rows->Add(new wxStaticText(this, wxID_ANY, labels[id - 1]), {row, column}, wxDefaultSpan, wxALIGN_CENTER_VERTICAL | wxALL, 3);
		auto* text = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(155, -1), wxTE_PROCESS_ENTER | wxTE_PROCESS_TAB);
		text->SetClientData(reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
		text->SetEditable(false);
		bind_hotkey_events(text);
		rows->Add(text, {row, column + 1}, wxDefaultSpan, wxEXPAND | wxALL, 3);
	}
	sizer->Add(rows, 0, wxALL, 4);
	m_motion = new wxCheckBox(this, wxID_ANY, _("Use controller motion for tilt"));
	m_motion->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
		if (auto guitar = m_guitar.lock()) guitar->SetMotionTilt(m_motion->IsChecked());
	});
	sizer->Add(m_motion, 0, wxALL, 6);
	auto* recenter = new wxButton(this, wxID_ANY, _("Set neutral tilt position"));
	recenter->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		if (auto guitar = m_guitar.lock()) guitar->RecenterMotion();
	});
	sizer->Add(recenter, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
	m_status = new wxStaticText(this, wxID_ANY, _("Motion tilt is off."));
	sizer->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
	auto* help = new wxStaticText(this, wxID_ANY,
		_("Click a field, then press a button/key or move a stick in the desired direction.\n"
		  "For right-stick strumming, bind its up/down directions to the two Strum fields.\n"
		  "For motion tilt, enable motion in the physical controller's Settings too.\n"
		  "Hold your normal playing pose steady to set neutral, then tilt 45 degrees\n"
		  "in either direction to activate. Button/axis tilt remains available.\n"
		  "Enable the guitar in Tools > Emulated USB Devices, then restart the game.\n"
		  "Only one virtual guitar is supported; keep GamePad on a separate controller tab."));
	sizer->Add(help, 0, wxALL, 6);
	SetSizerAndFit(sizer);
}

void GuitarHeroLiveInputPanel::load_controller(const EmulatedControllerPtr& controller)
{
	InputPanel::load_controller(controller);
	auto guitar = std::dynamic_pointer_cast<GuitarHeroLiveController>(controller);
	m_guitar = guitar;
	m_motion->SetValue(guitar && guitar->GetMotionTilt());
}

void GuitarHeroLiveInputPanel::on_timer(const EmulatedControllerPtr& emulated, const ControllerPtr& controller)
{
	InputPanel::on_timer(emulated, controller);
	if (auto guitar = m_guitar.lock())
	{
		const auto state = guitar->ReadState();
		wxString status;
		if (!guitar->GetMotionTilt()) status = _("Motion tilt is off.");
		else if (!guitar->has_motion()) status = _("No enabled motion source. Button/axis tilt still works.");
		else if (!guitar->MotionReady()) status = _("Hold steady in your normal playing pose to set neutral.");
		else status = state.tilt == 0xff ? _("Tilt active") : _("Motion ready - neutral");
		status += state.strumUp ? _(" | Strum up") : state.strumDown ? _(" | Strum down") : _(" | Strum neutral");
		if (m_status->GetLabel() != status) m_status->SetLabel(status);
	}
}
