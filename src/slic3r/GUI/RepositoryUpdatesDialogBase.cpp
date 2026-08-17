///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See RepositoryUpdatesDialogBase.hpp. This file deliberately contains only
// generic wx lifetime handling; plugin and preset wording stays in their own
// dialogs.

#include "RepositoryUpdatesDialogBase.hpp"

#include <wx/busyinfo.h>
#include <wx/msgdlg.h>

#include "libslic3r/Updater/UpdaterError.hpp"

#include "I18N.hpp"
#include "GUI.hpp"

namespace Slic3r::GUI {

RepositoryUpdatesDialogBase::RepositoryUpdatesDialogBase(wxWindow *parent, const wxString &title)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
}

RepositoryUpdatesDialogBase::~RepositoryUpdatesDialogBase()
{
    // Repository operations continue updating their model after this window
    // closes, but none of their queued continuations may touch its controls.
    m_operation_lifetime.reset();
}

void RepositoryUpdatesDialogBase::begin_repository_operation(wxWindow *action_area,
                                                               const wxString &message)
{
    m_repository_operation_in_progress = true;
    m_action_area = action_area;
    if (action_area != nullptr)
        action_area->Enable(false);
    m_wait_dialog = std::make_unique<wxBusyInfo>(message);
}

void RepositoryUpdatesDialogBase::finish_repository_operation(wxWindow *action_area)
{
    m_wait_dialog.reset();
    m_repository_operation_in_progress = false;
    wxWindow *area_to_enable = action_area != nullptr ? action_area : m_action_area;
    if (area_to_enable != nullptr)
        area_to_enable->Enable(true);
    m_action_area = nullptr;
}

bool RepositoryUpdatesDialogBase::repository_operation_in_progress() const
{
    return m_repository_operation_in_progress;
}

void RepositoryUpdatesDialogBase::show_repository_error(const std::string &message)
{
    wxMessageBox(from_u8(message), _L("Repository updates"), wxICON_ERROR);
}

void RepositoryUpdatesDialogBase::handle_repository_exception(std::exception_ptr exception) noexcept
{
    const UpdaterError error = make_updater_error_from_exception(std::move(exception));
    try {
        finish_repository_operation(nullptr);
        show_repository_error(error.detail.empty() ?
            "The updater encountered an unexpected error." : error.detail);
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Failed reporting an exception from a repository GUI operation.";
    }
}

} // namespace Slic3r::GUI
