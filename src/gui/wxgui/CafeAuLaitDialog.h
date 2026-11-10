#pragma once

#include <wx/dialog.h>
#include <wx/timer.h>
#include <filesystem>
#include "config/ImportRestart.h"

class wxTextCtrl;
class wxCheckBox;
class wxStaticText;
class wxButton;
class wxChoice;

class CafeAuLaitDialog final : public wxDialog
{
public:
    explicit CafeAuLaitDialog(wxWindow* parent);
    const ImportRestart::Target& GetRestartTarget() const { return m_restartTarget; }
private:
    void Start();
    void Poll();
    void Cancel();
    void RestartImport();
    void SetBusy(bool busy);
    std::filesystem::path m_support;
    std::filesystem::path m_job;
    std::filesystem::path m_import;
    ImportRestart::Target m_restartTarget;
    wxTextCtrl* m_address{};
    wxChoice* m_region{};
    wxTextCtrl* m_destination{};
    wxTextCtrl* m_status{};
    wxCheckBox* m_consent{};
    wxStaticText* m_pair{};
    wxButton* m_browse{};
    wxButton* m_start{};
    wxButton* m_cancel{};
    wxButton* m_open{};
    wxTimer m_timer;
    long m_pid{};
    int m_missingTicks{};
};
