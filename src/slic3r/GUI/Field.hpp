///|/ Copyright (c) Prusa Research 2017 - 2023 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Vojtěch Král @vojtechkral, Enrico Turri @enricoturri1966
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLIC3R_GUI_FIELD_HPP
#define SLIC3R_GUI_FIELD_HPP

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
    #include <wx/wx.h>
#endif

#include <memory>
#include <cstdint>
#include <functional>
#include <boost/any.hpp>

#include <wx/spinctrl.h>
#include <wx/bmpcbox.h>
#include <wx/clrpicker.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/Utils.hpp"

#include "GUI.hpp"
#include "wxExtensions.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/SpinInput.hpp"
#include "Widgets/TextInput.hpp"

#ifdef __WXMSW__
#define wxMSW true
#else
#define wxMSW false
#endif

namespace Slic3r { namespace GUI {

class Field;
using t_field = std::unique_ptr<Field>;
using t_kill_focus = std::function<void(const std::string&)>;
using t_change = std::function<void(const t_config_option_key&, const boost::any&)>;
using t_back_to_init = std::function<void(const std::string&)>;

wxString double_to_string(double const value, const int max_precision = 6);
wxString get_points_string(const std::vector<Vec2d>& values);
// return {invalid_val, out_of_range_val}
std::pair<bool, bool> get_strings_points(const wxString &str, double min, double max, std::vector<Vec2d> &out_values);

class UndoValueUIManager
{
	struct UndoValueUI {
		// Bitmap and Tooltip text for m_Undo_btn. The wxButton will be updated only if the new wxBitmap pointer differs from the currently rendered one.
		const ScalableBitmap* undo_bitmap{ nullptr };
		const wxString* undo_tooltip{ nullptr };
		// Bitmap and Tooltip text for m_Undo_to_sys_btn. The wxButton will be updated only if the new wxBitmap pointer differs from the currently rendered one.
		const ScalableBitmap* undo_to_sys_bitmap{ nullptr };
		const wxString* undo_to_sys_tooltip{ nullptr };
		// Color for Label. The wxColour will be updated only if the new wxColour pointer differs from the currently rendered one.
		const wxColour* label_color{ nullptr };
		// State of the blinker icon
		bool					blink{ false };

		bool 	set_undo_bitmap(const ScalableBitmap* bmp) {
			if (undo_bitmap != bmp) {
				undo_bitmap = bmp;
				return true;
			}
			return false;
		}

		bool 	set_undo_to_sys_bitmap(const ScalableBitmap* bmp) {
			if (undo_to_sys_bitmap != bmp) {
				undo_to_sys_bitmap = bmp;
				return true;
			}
			return false;
		}

		bool	set_label_colour(const wxColour* clr) {
			if (label_color != clr) {
				label_color = clr;
			}
			return false;
		}

		bool 	set_undo_tooltip(const wxString* tip) {
			if (undo_tooltip != tip) {
				undo_tooltip = tip;
				return true;
			}
			return false;
		}

		bool 	set_undo_to_sys_tooltip(const wxString* tip) {
			if (undo_to_sys_tooltip != tip) {
				undo_to_sys_tooltip = tip;
				return true;
			}
			return false;
		}
	};

	UndoValueUI m_undo_ui;

	struct EditValueUI {
		// Bitmap and Tooltip text for m_Edit_btn. The wxButton will be updated only if the new wxBitmap pointer differs from the currently rendered one.
		const ScalableBitmap*	bitmap{ nullptr };
		wxString				tooltip { wxEmptyString };

		bool 	set_bitmap(const ScalableBitmap* bmp) {
			if (bitmap != bmp) {
				bitmap = bmp;
				return true;
			}
			return false;
		}

		bool 	set_tooltip(const wxString& tip) {
			if (tooltip != tip) {
				tooltip = tip;
				return true;
			}
			return false;
		}
	};

	EditValueUI m_edit_ui;

public:
	UndoValueUIManager() {}
	~UndoValueUIManager() {}

	bool 	set_undo_bitmap(const ScalableBitmap* bmp)			{ return m_undo_ui.set_undo_bitmap(bmp); }
	bool 	set_undo_to_sys_bitmap(const ScalableBitmap* bmp)	{ return m_undo_ui.set_undo_to_sys_bitmap(bmp); }
	bool	set_label_colour(const wxColour* clr)				{ return m_undo_ui.set_label_colour(clr); }
	bool 	set_undo_tooltip(const wxString* tip)				{ return m_undo_ui.set_undo_tooltip(tip); }
	bool 	set_undo_to_sys_tooltip(const wxString* tip)		{ return m_undo_ui.set_undo_to_sys_tooltip(tip); }	

	bool 	set_edit_bitmap(const ScalableBitmap* bmp)			{ return m_edit_ui.set_bitmap(bmp); }
	bool 	set_edit_tooltip(const wxString& tip)				{ return m_edit_ui.set_tooltip(tip); }

	// ui items used for revert line value
	bool					has_undo_ui()			const { return m_undo_ui.undo_bitmap != nullptr; }
	const wxBitmapBundle&	undo_bitmap()			const { return m_undo_ui.undo_bitmap->bmp(); }
	const wxString*			undo_tooltip()			const { return m_undo_ui.undo_tooltip; }
	const wxBitmapBundle&	undo_to_sys_bitmap()	const { return m_undo_ui.undo_to_sys_bitmap->bmp(); }
	const wxString*			undo_to_sys_tooltip()	const { return m_undo_ui.undo_to_sys_tooltip; }
	const wxColour*			label_color()			const { return m_undo_ui.label_color; }

	// Extentions

	// Search blinker
	const bool				blink()					const { return m_undo_ui.blink; }
	bool*					get_blink_ptr()				  { return &m_undo_ui.blink; }

	// Edit field button
	bool					has_edit_ui()			const { return !m_edit_ui.tooltip.IsEmpty(); }
	const wxBitmapBundle*	edit_bitmap()			const { return &m_edit_ui.bitmap->bmp(); }
	const wxString*			edit_tooltip()			const { return &m_edit_ui.tooltip; }
};


class Field;
class RichTooltipTimer : public wxTimer
{
	Field*				m_field;
public:
	bool				m_is_rich_tooltip_ready = false;
	wxWindow*			m_current_rich_tooltip  = nullptr;
	wxWindow*			m_previous_focus		= nullptr;
	wxString			m_value;
	wxWindow*			m_window2				= nullptr; //for point
	wxWindow*			m_current_window		= nullptr; //for point
	RichTooltipTimer(Field* field) : m_field(field) {};

	void Notify() override;
};

class Field : public UndoValueUIManager
{
protected:
    // factory function to defer and enforce creation of derived type. 
	virtual void	PostInitialize();
    
    /// Finish constructing the Field's wxWidget-related properties, including setting its own sizer, etc.
    virtual void	BUILD() = 0;

    /// Call the attached on_kill_focus method. 
	//! It's important to use wxEvent instead of wxFocusEvent,
	//! in another case we can't unfocused control at all
	void			on_kill_focus();
    /// Call the attached on_change method. 
    void			on_change_field();

    class EnterPressed {
    public:
        EnterPressed(Field* field) : 
            m_parent(field){ m_parent->set_enter_pressed(true);  }
        ~EnterPressed()    { m_parent->set_enter_pressed(false); }
    private:
        Field* m_parent;
    };

public:
    /// Call the attached m_back_to_initial_value method. 
	void			on_back_to_initial_value();
    /// Call the attached m_back_to_sys_value method. 
	void			on_back_to_sys_value();
    /// Call the attached m_fn_edit_value method. 
	void			on_edit_value();

public:
    /// parent wx item, opportunity to refactor (probably not necessary - data duplication)
    wxWindow*		m_parent {nullptr};

    /// Function object to store callback passed in from owning object.
	t_kill_focus	m_on_kill_focus {nullptr};

	/// Function object to store callback passed in from owning object.
	t_change		m_on_change{ nullptr };

	/// Function object to store callback passed in from owning object.
	t_back_to_init	m_back_to_initial_value{ nullptr };
	t_back_to_init	m_back_to_sys_value{ nullptr };

	/// Callback function to edit field value 
	t_back_to_init	m_fn_edit_value{ nullptr };

	// This is used to avoid recursive invocation of the field change/update by wxWidgets.
    bool			m_disable_change_event {false};
    bool			m_is_modified_value {false};
    bool            m_is_nonsys_value{true};

    /// Copy of ConfigOption for deduction purposes
    const ConfigOptionDef			m_opt {ConfigOptionDef()};
	const t_config_option_key		m_opt_id;//! {""};
	int								m_opt_idx = -1;

	// for saving state
    bool                            m_is_enable{true};

	double							opt_height{ 0.0 };
	bool							parent_is_custom_ctrl{ false };

    /// Sets a value for this control.
    /// subclasses should overload with a specific version
    /// Postcondition: Method does not fire the on_change event.
    virtual void        set_any_value(const boost::any &value, bool change_event) = 0;
    virtual void        set_last_meaningful_value() {}
    virtual void        set_na_value() {}

    /// Gets a boost::any representing this control.
    /// subclasses should overload with a specific version
    virtual boost::any&	get_value() = 0;

    virtual void		enable() = 0;
    virtual void		disable() = 0;

	/// Fires the enable or disable function, based on the input.
    inline void			toggle(bool en) {
		m_is_enable = en;
		en ? enable() : disable();
	}
    inline bool is_enabled() const { return m_is_enable; }

	virtual wxString	get_tooltip_text(const wxString& default_string);
	// hack via richtooltip that are also hacked
	RichTooltipTimer	m_rich_tooltip_timer;
	virtual wxString	get_rich_tooltip_text(const wxString& default_string);
	virtual wxString	get_rich_tooltip_title(const wxString& default_string);
	void				set_tooltip(const wxString& default_string, wxWindow* window = nullptr);

    void				field_changed() { on_change_field(); }

    Field(const ConfigOptionDef& opt, const t_config_option_key& id) : m_opt(opt), m_opt_id(id), m_rich_tooltip_timer(this) {}
    Field(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : m_parent(parent), m_opt(opt), m_opt_id(id), m_rich_tooltip_timer(this) {}
    virtual ~Field();

    /// If you don't know what you are getting back, check both methods for nullptr. 
    virtual wxSizer*	getSizer()  { return nullptr; }
    virtual wxWindow*	getWindow() { return nullptr; }

    bool is_matched(const std::string &string, const std::string &pattern);

    /// Factory method for generating new derived classes.
    template<class T>
    static t_field Create(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id)// interface for creating shared objects
    {
        auto p = Slic3r::make_unique<T>(parent, opt, id);
        p->PostInitialize();
		return std::move(p); //!p;
    }

    virtual void msw_rescale();
    virtual void sys_color_changed();

    bool get_enter_pressed() const { return bEnterPressed; }
    void set_enter_pressed(bool pressed) { bEnterPressed = pressed; }

	// Values of width to alignments of fields
	static int def_width()			;
	static int def_width_wider()	;
	static int def_width_thinner()	;

protected:
	// current value
	boost::any			m_value;
	// last validated value
	wxString			m_last_validated_value;

    int                 m_em_unit;

    bool    bEnterPressed = false;

	inline static bool warn_zero_gapfillspeed = false;
    
	friend class OptionsGroup;
};

class TextField : public Field
{
    using Field::Field;
protected:
    TextField(const ConfigOptionDef &opt, const t_config_option_key &id) : Field(opt, id) {}
    TextField(wxWindow *parent, const ConfigOptionDef &opt, const t_config_option_key &id) : Field(parent, opt, id)
    {}
    ~TextField() {}

    void get_value_by_opt_type(wxString &str, const bool check_value = true);
    bool get_vector_value(const wxString &str, ConfigOptionVectorBase &reader);
    virtual void set_text_value(const std::string &str, bool change_event = false) = 0;

    // last meaningful value (can be whatever the child class want it to be)
    wxString m_last_meaningful_value;
};

/// Convenience function, accepts a const reference to t_field and checks to see whether 
/// or not both wx pointers are null.
inline bool is_bad_field(const t_field& obj) { return obj->getSizer() == nullptr && obj->getWindow() == nullptr; }

/// Covenience function to determine whether this field is a valid window field.
inline bool is_window_field(const t_field& obj) { return !is_bad_field(obj) && obj->getWindow() != nullptr && obj->getSizer() == nullptr; }

/// Covenience function to determine whether this field is a valid sizer field.
inline bool is_sizer_field(const t_field& obj) { return !is_bad_field(obj) && obj->getSizer() != nullptr; }

using text_ctrl = ::TextInput; //wxTextCtrl

class TextCtrl : public TextField {
    using TextField::TextField;
#ifdef __WXGTK__
	bool	bChangedValueEvent = true;
    void    change_field_value(wxEvent& event);
#endif //__WXGTK__

public:
    TextCtrl(const ConfigOptionDef &opt, const t_config_option_key &id) : TextField(opt, id) {}
	TextCtrl(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : TextField(parent, opt, id) {}
	~TextCtrl() {}

    void BUILD() override;
    bool value_was_changed();
    // Propagate value from field to the OptionGroupe and Config after kill_focus/ENTER
    void propagate_value();
    wxWindow* window {nullptr};

    void	set_text_value(const std::string &value, bool change_event = false) override;
	void	set_any_value(const boost::any& value, bool change_event = false) override;
    void    set_last_meaningful_value() override;
    void	set_na_value() override;

	boost::any&		get_value() override;

    void            msw_rescale() override;
    
    void			enable() override;
    void			disable() override;
    wxWindow* 		getWindow() override { return window; }
};

class CheckBox : public Field {
	using Field::Field;
    bool            m_is_na_val {false};
    // last meaningful value (can be whatever the child class want it to be)
    uint8_t         m_last_meaningful_value;
public:
	CheckBox(const ConfigOptionDef& opt, const t_config_option_key& id) : Field(opt, id) {}
	CheckBox(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : Field(parent, opt, id) {}
	~CheckBox() {}

	static wxWindow*	GetNewWin(wxWindow* parent, const wxString& label = wxEmptyString);
	static void			SetValue(wxWindow* win, bool value);
	static bool			GetValue(wxWindow* win);
	static void			Rescale(wxWindow* win);
	static void			SysColorChanged(wxWindow* win);

	wxWindow*		window{ nullptr };
	void			BUILD() override;

	void			set_bool_value(const bool value, bool change_event = false);
    void            set_any_value(const boost::any &value, bool change_event = false) override;
    void            set_last_meaningful_value() override;
	void            set_na_value() override;
	boost::any&		get_value() override;

    void            msw_rescale() override;
	void            sys_color_changed() override;

	void			enable() override;
	void			disable() override;
	wxWindow*		getWindow() override { return window; }

private:
	void SetValue(bool value);
	//bool GetValue();
};

class SpinCtrl : public Field {
	using Field::Field;
private:
	static const int UNDEF_VALUE = INT_MIN;
    int32_t          m_last_meaningful_value;

public:
	SpinCtrl(const ConfigOptionDef& opt, const t_config_option_key& id) : Field(opt, id), tmp_value(UNDEF_VALUE) {}
	SpinCtrl(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : Field(parent, opt, id), tmp_value(UNDEF_VALUE) {}
	~SpinCtrl() {}

	int32_t         tmp_value;

	wxWindow*		window{ nullptr };
	void			BUILD() override;
    /// Propagate value from field to the OptionGroupe and Config after kill_focus/ENTER
    void	        propagate_value() ;
/*
    void			set_text_value(const std::string& value, bool change_event = false) {
		m_disable_change_event = !change_event;
		dynamic_cast<::SpinInput*>(window)->SetValue(value);
		m_disable_change_event = false;
    }
    void            set_any_value(const boost::any &value, bool change_event = false) override {
		m_disable_change_event = !change_event;
		tmp_value = boost::any_cast<int>(value);
        m_value = value;
		dynamic_cast<::SpinInput*>(window)->SetValue(tmp_value);
		m_disable_change_event = false;
	}
*/
    void            set_any_value(const boost::any& value, bool change_event = false) override;
    void            set_last_meaningful_value() override;
    void            set_na_value() override;

	boost::any&		get_value() override;
/*
	boost::any&		get_value() override {
		int value = static_cast<::SpinInput*>(window)->GetValue();
		return m_value = value;
	}
*/
    void            msw_rescale() override;

	void			enable()  override { dynamic_cast<::SpinInput*>(window)->Enable(); }
	void			disable() override { dynamic_cast<::SpinInput*>(window)->Disable(); }
	wxWindow*		getWindow() override { return window; }
};

class Choice : public TextField
{
	using TextField::TextField;
public:
    Choice(const ConfigOptionDef &opt, const t_config_option_key &id) : TextField(opt, id) {}
    Choice(wxWindow *parent, const ConfigOptionDef &opt, const t_config_option_key &id) : TextField(parent, opt, id)
    {}
	~Choice() {}

	wxWindow*		window{ nullptr };
	void			BUILD() override;
	// Propagate value from field to the OptionGroupe and Config after kill_focus/ENTER
	void			propagate_value();

    /* Under OSX: wxBitmapComboBox->GetWindowStyle() returns some weard value, 
     * so let use a flag, which has TRUE value for a control without wxCB_READONLY style
     */
    bool            m_is_editable     { false };
    bool            m_is_dropped      { false };
    bool            m_suppress_scroll { false };
    int             m_last_selected   { wxNOT_FOUND };

	void			set_selection();
    void            set_text_value(const std::string &value, bool change_event = false);
    void            set_any_value(const boost::any &value, bool change_event = false) override;
	void			set_values(const std::vector<std::string> &values);
	void			set_values(const wxArrayString &values);
	boost::any&		get_value() override;

    void            msw_rescale() override;

	void			enable() override ;//{ dynamic_cast<wxBitmapComboBox*>(window)->Enable(); };
	void			disable() override;//{ dynamic_cast<wxBitmapComboBox*>(window)->Disable(); };
	wxWindow*		getWindow() override { return window; }

    void            suppress_scroll();
};

class ColourPicker : public Field {
	using Field::Field;

    void            set_undef_value(wxColourPickerCtrl* field);
public:
	ColourPicker(const ConfigOptionDef& opt, const t_config_option_key& id) : Field(opt, id) {}
	ColourPicker(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : Field(parent, opt, id) {}
	~ColourPicker() {}

	wxWindow*		window{ nullptr };
	void			BUILD()  override;

	void			set_text_value(const std::string& value, bool change_event = false) {
		m_disable_change_event = !change_event;
		dynamic_cast<wxColourPickerCtrl*>(window)->SetColour(value);
		m_disable_change_event = false;
	 	}
    void            set_any_value(const boost::any &value, bool change_event = false) override;
	boost::any&		get_value() override;
    void            msw_rescale() override;
    void            sys_color_changed() override;

    void			enable() override { dynamic_cast<wxColourPickerCtrl*>(window)->Enable(); }
    void			disable() override{ dynamic_cast<wxColourPickerCtrl*>(window)->Disable(); }
	wxWindow*		getWindow() override { return window; }
};

class GraphButton : public Field {
    using Field::Field;
    GraphData current_value;
public:
    GraphButton(const ConfigOptionDef& opt, const t_config_option_key& id) : Field(opt, id) {}
    GraphButton(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : Field(parent, opt, id) {}
    ~GraphButton() {}

    wxWindow*       window{ nullptr };
    void            BUILD()  override;

    void            set_any_value(const boost::any &value, bool change_event = false) override;
    boost::any&     get_value() override;
    void            msw_rescale() override;
    void            sys_color_changed() override;

    void            enable() override { dynamic_cast<wxButton*>(window)->Enable(); }
    void            disable() override{ dynamic_cast<wxButton*>(window)->Disable(); }
    wxWindow*       getWindow() override { return window; }
};

class PointCtrl : public Field {
	using Field::Field;
public:
	PointCtrl(const ConfigOptionDef& opt, const t_config_option_key& id) : Field(opt, id) {}
	PointCtrl(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : Field(parent, opt, id) {}
	~PointCtrl();

	wxSizer*		sizer{ nullptr };
	text_ctrl*		x_textctrl{ nullptr };
	text_ctrl*		y_textctrl{ nullptr };

	void			BUILD()  override;
	bool			value_was_changed(text_ctrl* win);
    // Propagate value from field to the OptionGroupe and Config after kill_focus/ENTER
    void            propagate_value(text_ctrl* win);
	void			set_vec2d_value(const Vec2d& value, bool change_event = false);
    void            set_any_value(const boost::any &value, bool change_event = false) override;
	boost::any&		get_value() override;

    void            msw_rescale() override;
	void            sys_color_changed() override;

	void			enable() override {
		x_textctrl->Enable();
		y_textctrl->Enable(); }
	void			disable() override{
		x_textctrl->Disable();
		y_textctrl->Disable(); }
	wxSizer*		getSizer() override { return sizer; }
	//for height
	wxWindow*		getWindow() override { return dynamic_cast<wxWindow*>(x_textctrl); }
};

class StaticText : public Field {
	using Field::Field;
public:
	StaticText(const ConfigOptionDef& opt, const t_config_option_key& id) : Field(opt, id) {}
	StaticText(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : Field(parent, opt, id) {}
	~StaticText() {}

	wxWindow*		window{ nullptr };
	void			BUILD()  override;

	void			set_text_value(const std::string& value, bool change_event = false) {
		m_disable_change_event = !change_event;
		dynamic_cast<wxStaticText*>(window)->SetLabel(wxString::FromUTF8(value.data()));
		m_disable_change_event = false;
	}
	void			set_any_value(const boost::any& value, bool change_event = false) override {
		m_disable_change_event = !change_event;
		dynamic_cast<wxStaticText*>(window)->SetLabel(boost::any_cast<wxString>(value));
		m_disable_change_event = false;
	}

	boost::any&		get_value()override { return m_value; }

    void            msw_rescale() override;

    void			enable() override { dynamic_cast<wxStaticText*>(window)->Enable(); }
    void			disable() override{ dynamic_cast<wxStaticText*>(window)->Disable(); }
	wxWindow*		getWindow() override { return window; }
};

class SliderCtrl : public Field {
	using Field::Field;
public:
	SliderCtrl(const ConfigOptionDef& opt, const t_config_option_key& id) : Field(opt, id) {}
	SliderCtrl(wxWindow* parent, const ConfigOptionDef& opt, const t_config_option_key& id) : Field(parent, opt, id) {}
	~SliderCtrl() {}

	wxSizer*		m_sizer{ nullptr };
	wxTextCtrl*		m_textctrl{ nullptr };
	wxSlider*		m_slider{ nullptr };

	int				m_scale = 10;

	void			BUILD()  override;

	void			set_int_value(const int value, bool change_event = false);
	void			set_any_value(const boost::any& value, bool change_event = false) override;
	boost::any&		get_value() override;

	void			enable() override {
		m_slider->Enable();
		m_textctrl->Enable();
		m_textctrl->SetEditable(true);
	}
	void			disable() override{
		m_slider->Disable();
		m_textctrl->Disable();
		m_textctrl->SetEditable(false);
	}
	wxSizer*		getSizer() override { return m_sizer; }
	wxWindow*		getWindow() override { return dynamic_cast<wxWindow*>(m_slider); }
};

} // GUI
} // Slic3r

#endif /* SLIC3R_GUI_FIELD_HPP */
