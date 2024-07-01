#ifndef slic3r_GUI_CalibrationPressureAdvDialog_hpp_
#define slic3r_GUI_CalibrationPressureAdvDialog_hpp_

#include "CalibrationAbstractDialog.hpp"
//pressure advance PressureAdv
namespace Slic3r { 
namespace GUI {

class CalibrationPressureAdvDialog : public CalibrationAbstractDialog
{

public:
    CalibrationPressureAdvDialog(GUI_App* app, MainFrame* mainframe) : CalibrationAbstractDialog(app, mainframe, "Pressure calibration") 
    { create(boost::filesystem::path("calibration") / "filament_pressure", "filament_pressure.html", wxSize(1600, 600)); Centre(wxBOTH);}
    virtual ~CalibrationPressureAdvDialog(){ }
    
protected:
    void create_buttons(wxStdDialogButtonSizer* sizer) override;
    void create_geometry(wxCommandEvent& event_args);
    double magical_scaling(double, double, double, double, double, double, double );

    //i've set choice boxes for now just to save me typing numbers in when i want to test it :)
    wxComboBox* firstPa;    //first layer PA -user manual entry
    wxComboBox* startPa;    //starting PA value -user manual entry
    //wxTextCtrl* firstPa;  //edit to suit for manual data entry,
    wxComboBox* endPa;      //ending PA value -user manual entry
    wxComboBox* paIncrement;//increment PA by this value -user manual entry~~ or have drop down box ?
    wxComboBox* erPa;       //extrusion role Pressure/Linear Advance -user choice select
    wxComboBox* nbRuns;
    wxCheckBox* enableST;   // checkbox for "smooth_time" - klipper only feature?

};

} // namespace GUI
} // namespace Slic3r

#endif
