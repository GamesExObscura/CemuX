// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Transport.h"
#include "LanDatagram.h"
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

inline bool ConfigureDirectLink(wxWindow* parent)
{
    wxDialog dialog(parent, wxID_ANY, _("Host Wii U/3DS LAN link (Internet optional)"), wxDefaultPosition,
                    wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* help = new wxStaticText(&dialog, wxID_ANY,
        _("For home LAN, share this PC's LAN IPv4 address and the passphrase with the other player. No router forwarding is needed on the same LAN.\n"
          "Optional Internet: share your router's public IPv4 and forward this UDP port to this PC. No router or firewall settings are changed automatically.\n"
          "Configure the link before starting the game. Passphrases are session-only and are not saved."));
    help->Wrap(540);
    outer->Add(help, 0, wxALL | wxEXPAND, 12);
    auto* fields = new wxFlexGridSizer(2, 8, 10);
    fields->AddGrowableCol(1);
    fields->Add(new wxStaticText(&dialog, wxID_ANY, _("Local bind IPv4")), 0, wxALIGN_CENTER_VERTICAL);
    auto* address = new wxTextCtrl(&dialog, wxID_ANY, "0.0.0.0");
    fields->Add(address, 1, wxEXPAND);
    fields->Add(new wxStaticText(&dialog, wxID_ANY, _("UDP port")), 0, wxALIGN_CENTER_VERTICAL);
    auto* port = new wxSpinCtrl(&dialog, wxID_ANY);
    port->SetRange(1024, 65535); port->SetValue(44660);
    fields->Add(port, 0);
    fields->Add(new wxStaticText(&dialog, wxID_ANY, _("Shared passphrase")), 0, wxALIGN_CENTER_VERTICAL);
    auto* phrase = new wxTextCtrl(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    phrase->SetMaxLength(256);
    try { phrase->SetValue(wxString::FromUTF8(WiiU3DSLink::GenerateDirectPassphrase().c_str())); }
    catch (const std::exception& e) { wxMessageBox(wxString::FromUTF8(e.what()), _("Direct link"), wxOK | wxICON_ERROR, parent); return false; }
    fields->Add(phrase, 1, wxEXPAND);
    outer->Add(fields, 0, wxLEFT | wxRIGHT | wxEXPAND, 12);
    auto* copy = new wxButton(&dialog, wxID_ANY, _("Copy passphrase before hosting"));
    copy->Bind(wxEVT_BUTTON, [phrase](wxCommandEvent&) {
        if (wxTheClipboard->Open()) { wxTheClipboard->SetData(new wxTextDataObject(phrase->GetValue())); wxTheClipboard->Close(); }
    });
    outer->Add(copy, 0, wxALL, 12);
    auto* hint = new wxStaticText(&dialog, wxID_ANY, _("0.0.0.0 listens on this PC's IPv4 interfaces. Use the generated passphrase or a strong phrase of at least 16 characters.\n"
        "Carrier-grade NAT may prevent hosting. This is not Pretendo or full-game Smash cross-play."));
    hint->Wrap(540); outer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);
    outer->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxEXPAND, 12);
    dialog.FindWindow(wxID_OK)->SetLabel(_("Host"));
    dialog.SetSizerAndFit(outer);
    while (dialog.ShowModal() == wxID_OK) {
        const auto ip = address->GetValue().Trim().Trim(false).ToStdString();
        if (WiiU3DSLink::GetTransport().StartDirect(WiiU3DSLink::Role::WiiU, ip,
                static_cast<std::uint16_t>(port->GetValue()), phrase->GetValue().ToStdString(wxConvUTF8))) {
            phrase->Clear(); return true;
        }
        wxMessageBox(wxString::FromUTF8(WiiU3DSLink::DescribeStatus(WiiU3DSLink::GetTransport().GetStatus()).c_str()),
                     _("Direct link"), wxOK | wxICON_ERROR, &dialog);
    }
    phrase->Clear(); return false;
}
