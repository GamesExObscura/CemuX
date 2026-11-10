#include "CafeAuLaitDialog.h"
#include "config/ActiveSettings.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "wxHelper.h"
#include <pugixml.hpp>
#include <fstream>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/process.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace
{
std::filesystem::path PowerShell()
{
    wxString windowsRoot;
    if (!wxGetEnv("SystemRoot", &windowsRoot) || windowsRoot.empty())
        throw std::runtime_error("Cannot locate Windows PowerShell.");
    return std::filesystem::path(windowsRoot.ToStdWstring()) /
        "System32/WindowsPowerShell/v1.0/powershell.exe";
}

long RunScript(const std::filesystem::path& script, const std::filesystem::path& job = {})
{
    const auto program = wxHelper::FromPath(PowerShell());
    const auto file = wxHelper::FromPath(script);
    const auto argument = wxHelper::FromPath(job);
    // Argument arrays avoid interpreting user addresses/paths as shell commands.
    const wchar_t* withJob[] = {program.wc_str(), L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
        L"-File", file.wc_str(), L"-JobDirectory", argument.wc_str(), nullptr};
    const wchar_t* withoutJob[] = {program.wc_str(), L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
        L"-File", file.wc_str(), nullptr};
    return wxExecute(job.empty() ? withoutJob : withJob, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE);
}
}

CafeAuLaitDialog::CafeAuLaitDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _("Import Wii U System - Cafe au Lait"), wxDefaultPosition,
        wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_timer(this)
{
    m_support = ActiveSettings::GetExecutablePath().parent_path() / "cafe-au-lait";
    const auto pointer = m_support / "tool-root.txt";
    std::error_code error;
    if (std::filesystem::is_regular_file(pointer, error) && std::filesystem::file_size(pointer, error) < 4096)
    {
        std::ifstream input(pointer);
        std::string path;
        std::getline(input, path);
        if (!path.empty()) m_support = std::filesystem::u8path(path);
    }
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* intro = new wxStaticText(this, wxID_ANY,
        _("Open Cafe au Lait on your Wii U and press A.\nDownload the full internal MLC and online files into a NEW, separate Cemu setup.\nThe console is read-only. Existing PC setups, games and saves are not overwritten."));
    layout->Add(intro, 0, wxALL, 14);
    auto* addressRow = new wxBoxSizer(wxHORIZONTAL);
    addressRow->Add(new wxStaticText(this, wxID_ANY, _("Wii U IPv4 address:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    m_address = new wxTextCtrl(this, wxID_ANY);
    addressRow->Add(m_address, 1);
    addressRow->Add(new wxStaticText(this, wxID_ANY, _("Region:")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);
    const wxString regions[] = {"Auto", "JPN", "USA", "EUR"};
    m_region = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 4, regions);
    m_region->SetSelection(0);
    addressRow->Add(m_region);
    layout->Add(addressRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
    layout->Add(new wxStaticText(this, wxID_ANY, _("PC storage folder (received dump + separate import):")), 0, wxLEFT | wxRIGHT, 14);
    auto* folderRow = new wxBoxSizer(wxHORIZONTAL);
    m_destination = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    m_browse = new wxButton(this, wxID_ANY, _("Choose..."));
    folderRow->Add(m_destination, 1, wxRIGHT, 8);
    folderRow->Add(m_browse);
    layout->Add(folderRow, 0, wxEXPAND | wxALL, 14);
    m_consent = new wxCheckBox(this, wxID_ANY,
        _("I trust this private LAN. Console/account credentials are transferred unencrypted."));
    layout->Add(m_consent, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
    m_pair = new wxStaticText(this, wxID_ANY, _("Compare the pairing code here with your Wii U before approving."));
    layout->Add(m_pair, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
    m_status = new wxTextCtrl(this, wxID_ANY, _("Nothing has been transferred. Existing setups will remain unchanged."),
        wxDefaultPosition, wxSize(650, 150), wxTE_MULTILINE | wxTE_READONLY);
    layout->Add(m_status, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_start = new wxButton(this, wxID_ANY, _("Download and prepare"));
    m_cancel = new wxButton(this, wxID_ANY, _("Cancel transfer"));
    m_open = new wxButton(this, wxID_ANY, _("Restart Cemu"));
    m_open->SetToolTip(_("Close this Cemu normally, then restart with the completed imported setup."));
    buttons->Add(m_start, 0, wxRIGHT, 8);
    buttons->Add(m_cancel, 0, wxRIGHT, 8);
    buttons->Add(m_open, 0, wxRIGHT, 8);
    buttons->Add(new wxButton(this, wxID_CLOSE, _("Close")));
    layout->Add(buttons, 0, wxALL, 14);
    SetSizerAndFit(layout);
    SetMinSize(GetSize());
    CentreOnParent();
    m_open->Disable();
    SetBusy(false);
    m_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxDirDialog chooser(this, _("Choose local storage for your private console setup"), wxEmptyString, wxDD_DIR_MUST_EXIST);
        if (chooser.ShowModal() == wxID_OK) m_destination->SetValue(chooser.GetPath());
    });
    m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Start(); });
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Cancel(); });
    m_open->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RestartImport(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Poll(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); }, wxID_CLOSE);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        if (m_pid) { Cancel(); event.Veto(); return; }
        EndModal(wxID_CLOSE);
    });
}

void CafeAuLaitDialog::SetBusy(bool busy)
{
    m_start->Enable(!busy);
    m_browse->Enable(!busy);
    m_address->Enable(!busy);
    m_region->Enable(!busy);
    m_consent->Enable(!busy);
    m_cancel->Enable(busy);
}

void CafeAuLaitDialog::Start()
{
    if (!m_consent->IsChecked() || m_address->IsEmpty() || m_destination->IsEmpty())
    {
        wxMessageBox(_("Enter the Wii U's private IPv4 address, choose local storage, and approve trusted-LAN transfer."), GetTitle(), wxOK | wxICON_INFORMATION, this);
        return;
    }
    try
    {
        if (!std::filesystem::is_regular_file(m_support / "pc/NativeImport.ps1"))
            throw std::runtime_error("Cafe au Lait worker is missing. Keep the private package together.");
        const auto temporary = wxFileName::CreateTempFileName("cemu-cafe-import-");
        if (temporary.empty()) throw std::runtime_error("Cannot create a private job directory.");
        m_job = std::filesystem::path(temporary.ToStdWstring());
        std::filesystem::remove(m_job);
        std::filesystem::create_directory(m_job);
        pugi::xml_document request;
        auto node = request.append_child("request");
        node.append_attribute("address") = m_address->GetValue().utf8_str().data();
        node.append_attribute("region") = m_region->GetStringSelection().utf8_str().data();
        node.append_attribute("storage") = m_destination->GetValue().utf8_str().data();
        node.append_attribute("parent") = static_cast<unsigned int>(wxGetProcessId());
        node.append_attribute("consent") = true;
        if (!request.save_file((m_job / "request.xml").c_str())) throw std::runtime_error("Cannot save import request.");
        m_pid = RunScript(m_support / "pc/NativeImport.ps1", m_job);
        if (m_pid <= 0) { m_pid = 0; throw std::runtime_error("Cannot start the hidden transfer worker."); }
        m_missingTicks = 0;
        m_import.clear();
        m_open->Disable();
        m_pair->SetLabel(_("Connecting; compare the code with your Wii U when it appears."));
        m_status->SetValue(_("Starting verified transfer backend..."));
        SetBusy(true);
        m_timer.Start(250);
    }
    catch (const std::exception& error)
    {
        m_status->SetValue(wxString::FromUTF8(error.what()));
    }
}

void CafeAuLaitDialog::Cancel()
{
    if (!m_pid) return;
    std::ofstream cancel(m_job / "cancel");
    cancel << "cancel";
    m_cancel->Disable();
    m_status->SetValue(_("Cancelling safely. Incomplete data will not be opened. Please wait..."));
}

void CafeAuLaitDialog::Poll()
{
    const auto statusPath = m_job / "status.xml";
    std::error_code error;
    if (std::filesystem::is_regular_file(statusPath, error) && std::filesystem::file_size(statusPath, error) <= 65536)
    {
        pugi::xml_document document;
        if (document.load_file(statusPath.c_str(), pugi::parse_default, pugi::encoding_utf8))
        {
            const auto status = document.child("status");
            m_status->ChangeValue(wxString::FromUTF8(status.child_value("message")));
            const wxString pair = wxString::FromUTF8(status.child_value("pair"));
            if (!pair.empty()) m_pair->SetLabel(_("Compare on Wii U: ") + pair);
            if (status.attribute("finished").as_bool())
            {
                m_timer.Stop();
                m_pid = 0;
                SetBusy(false);
                if (status.attribute("success").as_bool())
                {
                    m_import = std::filesystem::u8path(status.child_value("import"));
                    m_open->Enable(!m_import.empty());
                }
                return;
            }
        }
    }
    if (!wxProcess::Exists(static_cast<int>(m_pid)))
    {
        if (++m_missingTicks > 8)
        {
            m_timer.Stop(); m_pid = 0; SetBusy(false);
            m_status->SetValue(_("Transfer worker exited without a completed import. Do not use incomplete output. Existing setups are unchanged."));
        }
    }
    else m_missingTicks = 0;
}

void CafeAuLaitDialog::RestartImport()
{
    if (m_import.empty()) return;
    if (wxMessageBox(_("Exit the Wii U app and turn the console off before using its online account in Cemu.\n\nRestart Cemu with the imported setup? This window will close normally before the imported setup opens."),
        GetTitle(), wxYES_NO | wxICON_INFORMATION, this) != wxYES) return;
    try
    {
        m_restartTarget = ImportRestart::Validate(m_import, [](const std::filesystem::path& path) {
            const auto size = std::filesystem::file_size(path);
            if (!size || size > 256 * 1024 * 1024) throw std::runtime_error("Invalid Cemu executable size.");
            std::ifstream file(path, std::ios::binary);
            std::vector<unsigned char> bytes(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
                throw std::runtime_error("Cannot verify the imported executable.");
            NCrypto::CHash256 hash;
            NCrypto::GenerateHashSHA256(bytes.data(), bytes.size(), hash);
            constexpr char hex[] = "0123456789ABCDEF";
            std::string result;
            for (const auto value : hash.b) { result += hex[value >> 4]; result += hex[value & 15]; }
            return result;
        });
        pugi::xml_document settings;
        const auto config = m_import / "settings.xml";
        if (!std::filesystem::is_regular_file(config) || std::filesystem::file_size(config) > 2 * 1024 * 1024 ||
            !settings.load_file(config.c_str()) ||
            !std::filesystem::equivalent(std::filesystem::u8path(settings.child("content").child_value("mlc_path")), m_restartTarget.mlc))
            throw std::runtime_error("Imported settings no longer point to the verified MLC. Restart cancelled.");
        EndModal(wxID_APPLY);
    }
    catch (const std::exception& error)
    {
        wxMessageBox(wxString::FromUTF8(error.what()), GetTitle(), wxOK | wxICON_ERROR, this);
    }
}
