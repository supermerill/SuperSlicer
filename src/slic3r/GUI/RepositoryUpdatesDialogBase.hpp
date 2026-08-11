///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// RepositoryUpdatesDialogBase is the small GUI contract shared by update
// dialogs. Network callbacks may arrive outside the current event handler, so
// derived dialogs always use call_after_repository_operation() before they
// rebuild widgets or show an error.

#ifndef slic3r_GUI_RepositoryUpdatesDialogBase_hpp_
#define slic3r_GUI_RepositoryUpdatesDialogBase_hpp_

#include <functional>
#include <string>

#include <wx/dialog.h>

namespace Slic3r::GUI {

class RepositoryUpdatesDialogBase : public wxDialog {
protected:
    RepositoryUpdatesDialogBase(wxWindow *parent, const wxString &title);

    // Queue UI work through wx so asynchronous repository callbacks never
    // access controls after their originating HTTP handler has returned.
    void call_after_repository_operation(std::function<void()> operation);
    void show_repository_error(const std::string &message);
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_RepositoryUpdatesDialogBase_hpp_
