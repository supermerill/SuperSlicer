#ifndef slic3r_GUI_CalibrationOverBridgeDialog_hpp_
#define slic3r_GUI_CalibrationOverBridgeDialog_hpp_

#include "CalibrationAbstractDialog.hpp"
#include "Jobs/Worker.hpp"
#include "Jobs/WindowWorker.hpp"
#include "Jobs/BoostThreadWorker.hpp"

namespace Slic3r { 
namespace GUI {

class CalibrationOverBridgeDialog : public CalibrationAbstractDialog
{

public:
    CalibrationOverBridgeDialog(GUI_App *app, MainFrame *mainframe)
        : CalibrationAbstractDialog(app, mainframe, "Ironing pattern calibration")
    {
        create(boost::filesystem::path("calibration") / "over-bridge_tuning", "over-bridge_tuning.html",
               wxSize(900, 500));
        m_worker = std::make_unique<WindowWorker<BoostThreadWorker>>(this, nullptr, "arrange_worker");
    }
    virtual ~CalibrationOverBridgeDialog() { }
    
protected:
    void create_buttons(wxStdDialogButtonSizer* sizer) override;
    void create_geometry1(wxCommandEvent& event_args);
    void create_geometry2(wxCommandEvent& event_args);
    void create_geometry(bool over_bridge);

    std::unique_ptr<Worker> m_worker;
};

} // namespace GUI
} // namespace Slic3r

#endif
