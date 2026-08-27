///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2023 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_ButtonsDescription_hpp
#define slic3r_ButtonsDescription_hpp

#include <vector>

#include <wx/bmpbndl.h>
#include <wx/dialog.h>

#include "libslic3r/Config/AppConfig.hpp"

#include "BitmapComboBox.hpp"
class ScalableBitmap;
class wxColourPickerCtrl;

namespace Slic3r {

namespace GUI {

class BitmapCache;

//disabled for Susi, we can switch the entire gui look&feel, not only these icons.
#ifdef GUI_TAG_PALETTE
// ---------------------------------
// ***  PaletteComboBox  ***
// ---------------------------------
// BitmapComboBox used to palets list in GUI Preferences
class ModePaletteComboBox : public BitmapComboBox
{
public:
    ModePaletteComboBox(wxWindow* parent);
	~ModePaletteComboBox() = default;

	void UpdateSelection(const std::vector<wxColour>& palette_in);

protected:
    // Caching bitmaps for the all bitmaps, used in preset comboboxes
    static BitmapCache&		bitmap_cache();
    wxBitmapBundle*			get_bmp( const std::vector<std::string>& palette);
};
#endif
namespace GUI_Descriptions {

struct ButtonEntry {
	ButtonEntry(ScalableBitmap *bitmap, const std::string &symbol, const std::string &explanation) : bitmap(bitmap), symbol(symbol), explanation(explanation) {}

	ScalableBitmap *bitmap;
	std::string     symbol;
	std::string   	explanation;
};

class Dialog : public wxDialog
{
	std::vector<ButtonEntry> m_entries;
	wxColourPickerCtrl* default_colour{ nullptr };
	wxColourPickerCtrl* sys_colour{ nullptr };
	wxColourPickerCtrl* mod_colour{ nullptr };
	wxColourPickerCtrl* phony_colour{ nullptr };

	//note: not thread-safe, dangerous container.
	std::map<ConfigOptionMode, wxColourPickerCtrl*> tags;
#ifdef GUI_TAG_PALETTE
	std::vector<wxColour> mode_palette;
#endif
public:

	Dialog(wxWindow* parent, const std::vector<ButtonEntry> &entries);
	~Dialog() {}
};

extern void FillSizerWithTextColorDescriptions(wxSizer* sizer, wxWindow* parent, 
    wxColourPickerCtrl** default_colour, wxColourPickerCtrl** sys_colour, wxColourPickerCtrl** mod_colour, wxColourPickerCtrl** phony_colour);
extern void FillSizerWithModeColorDescriptions(wxSizer* sizer, wxWindow* parent,
		                                       std::vector<std::pair<wxColourPickerCtrl**, AppConfig::Tag>> clr_pickers_2_color);
} // GUI_Descriptions

} // GUI
} // Slic3r


#endif 

