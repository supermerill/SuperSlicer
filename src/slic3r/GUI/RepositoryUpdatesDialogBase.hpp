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
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/event.h>

#include <boost/log/trivial.hpp>

class wxBusyInfo;

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
            if (wxTheApp == nullptr)
                return;
            try {
                wxTheApp->CallAfter(
                    [lifetime, dialog_pointer, operation = std::move(operation), result = std::move(result)]() mutable {
                        const std::shared_ptr<RepositoryOperationLifetime> keep_alive = lifetime.lock();
                        if (!keep_alive)
                            return;
                        try {
                            operation(*dialog_pointer, std::move(result));
                        } catch (...) {
                            dialog_pointer->handle_repository_exception(std::current_exception());
                        }
                    });
            } catch (const std::exception &error) {
                BOOST_LOG_TRIVIAL(error) << "Failed posting repository callback to wx: " << error.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Failed posting repository callback to wx with an unknown exception.";
            }
        };
    }

    // Wrap synchronous wx handlers which start updater work. File selectors
    // and validation setup still execute on wx, so this boundary prevents an
    // unexpected setup exception from escaping through the event loop.
    template<class Operation>
    void run_repository_gui_action(Operation operation) noexcept
    {
        try {
            operation();
        } catch (...) {
            handle_repository_exception(std::current_exception());
        }
    }

    // Bind a button so its complete synchronous handler is covered by the wx
    // exception boundary above. Asynchronous results still use
    // repository_operation_callback() when they return later.
    template<class Handler>
    void bind_repository_action(wxWindow &control, Handler handler)
    {
        control.Bind(wxEVT_BUTTON, [this, handler = std::move(handler)](wxCommandEvent &event) mutable {
            run_repository_gui_action([&handler, &event] { handler(event); });
        });
    }

    // Keep the dialog responsive while preventing another repository mutation
    // from starting through the same action area. Rebuilt controls consult
    // repository_operation_in_progress() to preserve this disabled state.
    void begin_repository_operation(wxWindow *action_area, const wxString &message);
    void finish_repository_operation(wxWindow *action_area);
    bool repository_operation_in_progress() const;

    void show_repository_error(const std::string &message);

private:
    void handle_repository_exception(std::exception_ptr exception) noexcept;

    // Only the dialog owns this state. Callbacks keep weak references, so
    // resetting it invalidates every pending GUI continuation at once.
    struct RepositoryOperationLifetime {};
    std::shared_ptr<RepositoryOperationLifetime> m_operation_lifetime =
        std::make_shared<RepositoryOperationLifetime>();
    std::unique_ptr<wxBusyInfo> m_wait_dialog;
    wxWindow *m_action_area = nullptr;
    bool m_repository_operation_in_progress = false;
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_RepositoryUpdatesDialogBase_hpp_
