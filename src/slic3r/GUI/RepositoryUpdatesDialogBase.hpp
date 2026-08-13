///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// RepositoryUpdatesDialogBase is the small GUI contract shared by update
// dialogs. Repository work may finish on an HTTP worker after the user has
// closed its window. Derived dialogs create callbacks with
// repository_operation_callback(): it transfers the result to the wx thread
// and silently drops GUI work when the originating dialog no longer exists.

#ifndef slic3r_GUI_RepositoryUpdatesDialogBase_hpp_
#define slic3r_GUI_RepositoryUpdatesDialogBase_hpp_

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <wx/app.h>
#include <wx/dialog.h>

namespace Slic3r::GUI {

class RepositoryUpdatesDialogBase : public wxDialog {
public:
    ~RepositoryUpdatesDialogBase() override;

protected:
    RepositoryUpdatesDialogBase(wxWindow *parent, const wxString &title);

    // Build the callback passed to an updater operation. The updater may call
    // it from any thread and after the dialog has closed. The raw pointer is
    // only dereferenced by wx after the lifetime token has been locked; wx
    // windows are destroyed on that same GUI thread, so destruction cannot
    // race the operation after this check succeeds.
    template<class Result, class Dialog, class Operation>
    std::function<void(Result)> repository_operation_callback(Dialog &dialog, Operation operation)
    {
        const std::weak_ptr<RepositoryOperationLifetime> lifetime = m_operation_lifetime;
        Dialog *dialog_pointer = &dialog;
        return [lifetime, dialog_pointer, operation = std::move(operation)](Result result) mutable {
            wxTheApp->CallAfter(
                [lifetime, dialog_pointer, operation = std::move(operation), result = std::move(result)]() mutable {
                    const std::shared_ptr<RepositoryOperationLifetime> keep_alive = lifetime.lock();
                    if (!keep_alive)
                        return;
                    operation(*dialog_pointer, std::move(result));
                });
        };
    }

    void show_repository_error(const std::string &message);

private:
    // Only the dialog owns this state. Callbacks keep weak references, so
    // resetting it invalidates every pending GUI continuation at once.
    struct RepositoryOperationLifetime {};
    std::shared_ptr<RepositoryOperationLifetime> m_operation_lifetime =
        std::make_shared<RepositoryOperationLifetime>();
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_RepositoryUpdatesDialogBase_hpp_
