#include "GUI_Init.hpp"

#include "libslic3r/AppConfig.hpp" 
#include "libslic3r/BlacklistedLibraryCheck.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/InstanceCheck.hpp" 
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/I18N.hpp"


// To show a message box if GUI initialization ends up with an exception thrown.
#include <wx/msgdlg.h>

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/convert.hpp>

#if __APPLE__
    #include <signal.h>
#endif // __APPLE__

namespace Slic3r {
namespace GUI {

#ifdef WIN32
    void test_win32_dll_loaded(AppConfig* appconf) {
        // Notify user that a blacklisted DLL was injected into PrusaSlicer process (for example Nahimic, see GH #5573).
        // We hope that if a DLL is being injected into a PrusaSlicer process, it happens at the very start of the application,
        // thus we shall detect them now.
        if (appconf->get("check_blacklisted_library") == "1") {
            if (BlacklistedLibraryCheck::get_instance().perform_check()) {
                std::wstring text = (boost::wformat(L"Following DLLs have been injected into the %1% process:") % SLIC3R_APP_NAME).str() + L"\n\n";
                text += BlacklistedLibraryCheck::get_instance().get_blacklisted_string();
                text += L"\n\n" +
                    (boost::wformat(L"%1% is known to not run correctly with these DLLs injected. "
                        L"We suggest stopping or uninstalling these services if you experience "
                        L"crashes or unexpected behaviour while using %2%.\n"
                        L"For example, ASUS Sonic Studio injects a Nahimic driver, which makes %3% "
                        L"to crash on a secondary monitor, see PrusaSlicer github issue #5573") % SLIC3R_APP_NAME % SLIC3R_APP_NAME % SLIC3R_APP_NAME).str();
                MessageBoxW(NULL, text.c_str(), L"Warning"/*L"Incompatible library found"*/, MB_OK);
            }
        }
    }
#endif

int GUI_Run(GUI_InitParams &params)
{
#if __APPLE__
    // On OSX, we use boost::process::spawn() to launch new instances of PrusaSlicer from another PrusaSlicer.
    // boost::process::spawn() sets SIGCHLD to SIGIGN for the child process, thus if a child PrusaSlicer spawns another
    // subprocess and the subrocess dies, the child PrusaSlicer will not receive information on end of subprocess
    // (posix waitpid() call will always fail).
    // https://jmmv.dev/2008/10/boostprocess-and-sigchld.html
    // The child instance of PrusaSlicer has to reset SIGCHLD to its default, so that posix waitpid() and similar continue to work.
    // See GH issue #5507
    signal(SIGCHLD, SIG_DFL);
#endif // __APPLE__

    try {
        GUI::GUI_App* gui = new GUI::GUI_App(params.start_as_gcodeviewer ? GUI::GUI_App::EAppMode::GCodeViewer : GUI::GUI_App::EAppMode::Editor);
        if (gui->get_app_mode() != GUI::GUI_App::EAppMode::GCodeViewer) {
            // G-code viewer is currently not performing instance check, a new G-code viewer is started every time.
            bool gui_single_instance_setting = gui->app_config->get("single_instance") == "1";
            if (Slic3r::instance_check(params.argc, params.argv, gui_single_instance_setting)) {
                //TODO: do we have delete gui and other stuff?
                return -1;
            }
        }

//      gui->autosave = m_config.opt_string("autosave");
        GUI::GUI_App::SetInstance(gui);
        gui->init_params = &params;

#ifdef WIN32
        test_win32_dll_loaded(gui->app_config.get());
#endif

        return wxEntry(params.argc, params.argv);
    } catch (const Slic3r::Exception &ex) {
        boost::nowide::cerr << ex.what() << std::endl;
        wxMessageBox(boost::nowide::widen(ex.what()), wxString::Format(_L("%s GUI initialization failed"), SLIC3R_APP_NAME), wxICON_STOP);
    } catch (const std::exception &ex) {
        boost::nowide::cerr << SLIC3R_APP_NAME << " GUI initialization failed: " << ex.what() << std::endl;
        wxMessageBox(format_wxstr(_L("Fatal error, exception catched: %1%"), ex.what()), wxString::Format(_L("%s GUI initialization failed"), SLIC3R_APP_NAME), wxICON_STOP);
    }

    // error
    return 1;
}
    
}
}
