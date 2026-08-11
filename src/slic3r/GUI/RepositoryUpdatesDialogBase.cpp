///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See RepositoryUpdatesDialogBase.hpp. This file deliberately contains only
// generic wx lifetime handling; plugin and preset wording stays in their own
// dialogs.

#include "RepositoryUpdatesDialogBase.hpp"

#include <wx/msgdlg.h>

#include "I18N.hpp"
#include "GUI.hpp"

namespace Slic3r::GUI {

RepositoryUpdatesDialogBase::RepositoryUpdatesDialogBase(wxWindow *parent, const wxString &title)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
}

void RepositoryUpdatesDialogBase::call_after_repository_operation(std::function<void()> operation)
{
    CallAfter(std::move(operation));
}

void RepositoryUpdatesDialogBase::show_repository_error(const std::string &message)
{
    wxMessageBox(from_u8(message), _L("Repository updates"), wxICON_ERROR);
}

} // namespace Slic3r::GUI
