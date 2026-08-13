///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Superslicer 2025 DUrand remi @supermerill
///|/ Copyright (c) Prusa Research 2018 - 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav, Lukáš Matěna @lukasmatena, Lukáš Hejl @hejllukas, Vojtěch Král @vojtechkral, Vojtěch Bubník @bubnikv
///|/ Copyright (c) 2020 Ondřej Nový @onovy
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "UpdateDialogs.hpp"

#include <algorithm>
#include <cstring>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/format.hpp>
#include <boost/nowide/convert.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dirdlg.h>
#include <wx/event.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

#include "ConfigWizard.hpp"
#include "format.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "UpdaterErrorMessages.hpp"
#include "slic3r/Config/Snapshot.hpp"
#include "slic3r/Utils/AppUpdater.hpp"
#include "libslic3r/Updater/Http.hpp"
#include "UnsavedChangesDialog.hpp"
#include "wxExtensions.hpp"
namespace Slic3r {
namespace GUI {


static const char* URL_CHANGELOG = "https://github.com/" SLIC3R_GITHUB "/releases";
static const char* URL_DOWNLOAD = "https://github.com/" SLIC3R_GITHUB "/releases";
static const char* URL_DEV = "https://github.com/" SLIC3R_GITHUB "/releases/tag/%1%";

static const std::string CONFIG_UPDATE_WIKI_URL("https://github.com/prusa3d/PrusaSlicer/wiki/Slic3r-PE-1.40-configuration-update");


// MsgUpdateSlic3r

MsgUpdateSlic3r::MsgUpdateSlic3r(const Semver &ver_current, const Semver &ver_online)
	: MsgDialog(nullptr, _(L("Update available")), wxString::Format(_(L("New version of %s is available")), SLIC3R_APP_NAME))
{
	const bool dev_version = true;// ver_online.prerelease() != nullptr; // Slic3r is always a dev version

	auto *versions = new wxFlexGridSizer(2, 0, VERT_SPACING);
	versions->Add(new wxStaticText(this, wxID_ANY, _(L("Current version:"))));
	versions->Add(new wxStaticText(this, wxID_ANY, ver_current.to_string()));
	versions->Add(new wxStaticText(this, wxID_ANY, _(L("New version:"))));
	versions->Add(new wxStaticText(this, wxID_ANY, ver_online.to_string()));
	content_sizer->Add(versions);
	content_sizer->AddSpacer(VERT_SPACING);

	if (dev_version) {
		const std::string url = (boost::format(URL_DEV) % ver_online.to_string()).str();
		const wxString url_wx = from_u8(url);
		auto *link = new wxHyperlinkCtrl(this, wxID_ANY, _(L("Changelog & Download")), url_wx);
		content_sizer->Add(link);
	} else {
		const auto lang_code = wxGetApp().current_language_code_safe().ToStdString();

		const std::string url_log = (boost::format(URL_CHANGELOG) % lang_code).str();
		const wxString url_log_wx = from_u8(url_log);
		auto *link_log = new wxHyperlinkCtrl(this, wxID_ANY, _(L("Open changelog page")), url_log_wx);
		link_log->Bind(wxEVT_HYPERLINK, &MsgUpdateSlic3r::on_hyperlink, this);
		content_sizer->Add(link_log);

		const std::string url_dw = (boost::format(URL_DOWNLOAD) % lang_code).str();
		const wxString url_dw_wx = from_u8(url_dw);
		auto *link_dw = new wxHyperlinkCtrl(this, wxID_ANY, _(L("Open download page")), url_dw_wx);
		link_dw->Bind(wxEVT_HYPERLINK, &MsgUpdateSlic3r::on_hyperlink, this);
		content_sizer->Add(link_dw);
	}

	content_sizer->AddSpacer(2*VERT_SPACING);

	cbox = new wxCheckBox(this, wxID_ANY, _(L("Don't notify about new releases any more")));
	content_sizer->Add(cbox);
	content_sizer->AddSpacer(VERT_SPACING);

	finalize();
}

MsgUpdateSlic3r::~MsgUpdateSlic3r() {}

void MsgUpdateSlic3r::on_hyperlink(wxHyperlinkEvent& evt)
{
	wxGetApp().open_browser_with_warning_dialog(evt.GetURL());
}

bool MsgUpdateSlic3r::disable_version_check() const
{
	return cbox->GetValue();
}

 wxSize AppUpdateAvailableDialog::AUAD_size;
// AppUpdater
AppUpdateAvailableDialog::AppUpdateAvailableDialog(const Semver& ver_current, const Semver& ver_online, bool from_user)
	: MsgDialog(nullptr, _(L("App Update available")), wxString::Format(_(L("New version of %s is available.\nDo you wish to download it?")), SLIC3R_APP_NAME))
{
	auto* versions = new wxFlexGridSizer(1, 0, VERT_SPACING);
	versions->Add(new wxStaticText(this, wxID_ANY, _(L("Current version:"))));
	versions->Add(new wxStaticText(this, wxID_ANY, ver_current.to_string()));
	versions->Add(new wxStaticText(this, wxID_ANY, _(L("New version:"))));
	versions->Add(new wxStaticText(this, wxID_ANY, ver_online.to_string()));
	content_sizer->Add(versions);
	content_sizer->AddSpacer(VERT_SPACING);

	if(!from_user) {
		cbox = new wxCheckBox(this, wxID_ANY, _(L("Don't notify about new releases any more")));
		content_sizer->Add(cbox);
	}
	content_sizer->AddSpacer(VERT_SPACING);
	
	AUAD_size = content_sizer->GetSize();
	
    
	add_button(wxID_NO);
	add_button(wxID_CANCEL);
    
	SetButtonLabel(wxID_OK, _L("Next"));
	SetButtonLabel(wxID_NO, _L("Skip"));

	finalize();
}

AppUpdateAvailableDialog::~AppUpdateAvailableDialog() {}


bool AppUpdateAvailableDialog::disable_version_check() const
{
	if (!cbox)
		return false;
	return cbox->GetValue();
}

// AppUpdateDownloadDialog
AppUpdateDownloadDialog::AppUpdateDownloadDialog( const Semver& ver_online, boost::filesystem::path& path)
	: MsgDialog(nullptr, _L("App Update download"), format_wxstr(_L("New version of %1% is available."), SLIC3R_APP_NAME))
{
	auto* versions = new wxFlexGridSizer(2, 0, VERT_SPACING);
	versions->Add(new wxStaticText(this, wxID_ANY, _L("New version") + ":"));
	versions->Add(new wxStaticText(this, wxID_ANY, ver_online.to_string()));
	content_sizer->Add(versions);
	content_sizer->AddSpacer(VERT_SPACING);
#ifndef __linux__
#ifdef _WIN32
    cbox_replace = new wxCheckBox(this, wxID_ANY, _(L("Upgrade current installation")));
    content_sizer->Add(cbox_replace);
    cbox_replace->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) { this->cbox_run->Enable(!event.IsChecked()); });
    cbox_replace->SetToolTip(_L(
        "This option makes the slicer download the zip bundle instead of the executable msi, extracting the files "
        "and replacing the current ones by the new ones. Note that the user need to have the right to write on the "
        "current directory, if not this option is disabled. It will also delete the downloaded zip after the upgrade "
        "if it succeed."));
    // test if possible
    bool can_write_install = false;
    try {
        boost::filesystem::path my_dir = binary_file().parent_path();
        boost::filesystem::create_directory(my_dir / "test_writep");
        boost::filesystem::remove(my_dir / "test_writep");
        can_write_install = true;
    } catch (std::exception) {
    }
    cbox_replace->Enable(can_write_install);
#endif
    cbox_run = new wxCheckBox(this, wxID_ANY, _(L("Run installer after download. (Otherwise file explorer will be opened)")));
    content_sizer->Add(cbox_run);
    cbox_run->SetToolTip(
        _L("This option makes the slicer download the latest release and execute it (.msi on windows, .dmg on macos)."));
#endif
	content_sizer->AddSpacer(VERT_SPACING);
	content_sizer->AddSpacer(VERT_SPACING);
	content_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Target directory") + ":"));
	content_sizer->AddSpacer(VERT_SPACING);
	txtctrl_path = new wxTextCtrl(this, wxID_ANY, GUI::format_wxstr(path.parent_path().string()));
    txtctrl_path->SetToolTip(
        _L("The directory the release is downloded to."));
	filename = GUI::format_wxstr(path.filename().string());
	content_sizer->Add(txtctrl_path, 1, wxEXPAND);
	content_sizer->AddSpacer(VERT_SPACING);
	
	wxButton* btn = new wxButton(this, wxID_ANY, _L("Select directory"));
	content_sizer->Add(btn/*, 1, wxEXPAND*/);

	// button to open file dialog
	btn->Bind(wxEVT_BUTTON, ([this, path](wxCommandEvent& e) {
		std::string extension = path.filename().extension().string();
		wxString wildcard;
		if (!extension.empty()) {
			extension = extension.substr(1);
			wxString wxext = boost::nowide::widen(extension);
			wildcard = GUI::format_wxstr("%1% Files (*.%2%)|*.%2%", wxext.Upper(), wxext);
		}
		boost::system::error_code ec;
		boost::filesystem::path dir = boost::filesystem::absolute(into_path(GUI::format(txtctrl_path->GetValue())), ec);
		if (ec)
			dir = GUI::format(txtctrl_path->GetValue());
		wxDirDialog save_dlg(
			this
			, _L("Select directory") + ":"
			, GUI::format_wxstr(dir.string())
			/*
			, filename //boost::nowide::widen(AppUpdater::get_filename_from_url(txtctrl_path->GetValue().ToUTF8().data()))
			, wildcard
			, wxFD_SAVE | wxFD_OVERWRITE_PROMPT*/
		);
		if (save_dlg.ShowModal() == wxID_OK) {
			txtctrl_path->SetValue(save_dlg.GetPath());
		}
	}));

	content_sizer->SetMinSize(AppUpdateAvailableDialog::AUAD_size);

	add_button(wxID_CANCEL);

	if (auto* btn_ok = get_button(wxID_OK); btn_ok != NULL) {
		btn_ok->SetLabel(_L("Download"));
		btn_ok->Bind(wxEVT_BUTTON, ([this, path](wxCommandEvent& e){
			boost::system::error_code ec;
			std::string input = GUI::into_u8(txtctrl_path->GetValue());
			boost::filesystem::path dir = boost::filesystem::absolute(into_path(input), ec);
			if (ec)
				dir = into_path(input);
			bool show_change = (dir.string() != input);
			boost::filesystem::path path = dir / GUI::format(filename);
			ec.clear();
			if (dir.string().empty()) {
				MessageDialog msgdlg(nullptr, _L("Directory path is empty."), _L("Notice"), wxOK);
				msgdlg.ShowModal();
				return;
			}
			ec.clear();
			if (!boost::filesystem::exists(dir, ec) || !boost::filesystem::is_directory(dir,ec) || ec) {
				ec.clear();
				if (!boost::filesystem::exists(dir.parent_path(), ec) || !boost::filesystem::is_directory(dir.parent_path(), ec) || ec) {
					MessageDialog msgdlg(nullptr, _L("Directory path is incorrect."), _L("Notice"), wxOK);
					msgdlg.ShowModal();
					return;
				}
				show_change = false;
				MessageDialog msgdlg(nullptr, GUI::format_wxstr(_L("Directory %1% doesn't exists. Do you wish to create it?"), dir.string()), _L("Notice"), wxYES_NO);
				if (msgdlg.ShowModal() != wxID_YES)
					return;
				ec.clear();
				if(!boost::filesystem::create_directory(dir, ec) || ec) {
					MessageDialog msgdlg(nullptr, _L("Failed to create directory."), _L("Notice"), wxOK);
					msgdlg.ShowModal();
					return;
				}
			}
			if (boost::filesystem::exists(path)) {
				show_change = false;
				MessageDialog msgdlg(nullptr, GUI::format_wxstr(_L("File %1% already exists. Do you wish to overwrite it?"), path.string()),_L("Notice"), wxYES_NO);
				if (msgdlg.ShowModal() != wxID_YES)
					return;
			}
			if (show_change) {
				MessageDialog msgdlg(nullptr, GUI::format_wxstr(_L("Download path is %1%. Do you wish to continue?"), path.string()), _L("Notice"), wxYES_NO);
				if (msgdlg.ShowModal() != wxID_YES)
					return;
			}
			this->EndModal(wxID_OK);
		}));
	}


	finalize();
}

AppUpdateDownloadDialog::~AppUpdateDownloadDialog() {}


bool AppUpdateDownloadDialog::run_after_download() const
{
#ifndef __linux__
    return cbox_run ? cbox_run->GetValue() : false;
#endif
    return false;
}

bool AppUpdateDownloadDialog::replace_current_after_download() const
{
#ifdef _WIN32
    return cbox_replace ? cbox_replace->GetValue() : false;
#endif
    return false;
}

boost::filesystem::path AppUpdateDownloadDialog::get_download_path() const
{
	boost::system::error_code ec;
	std::string input = GUI::into_u8(txtctrl_path->GetValue());
	boost::filesystem::path dir = boost::filesystem::absolute(into_path(input), ec);
	if (ec)
		dir = into_path(input);
	return dir / GUI::format(filename);
}

// MsgUpdateConfig

MsgUpdateConfig::MsgUpdateConfig(const std::vector<Update> &updates, bool force_before_wizard/* = false*/) :
	MsgDialog(nullptr, force_before_wizard ? _L("Opening Configuration Wizard") : _L("Configuration update"), 
					   force_before_wizard ? wxString::Format(_L("%s is not using the newest configuration available.\n"
												"Configuration Wizard may not offer the latest printers, filaments and SLA materials to be installed. "), SLIC3R_APP_NAME) : 
											 _L("Configuration update is available"), wxICON_ERROR)
{
	auto *text = new wxStaticText(this, wxID_ANY, _(L(
		"Would you like to install it?\n\n"
		"Note that a full configuration snapshot will be created first. It can then be restored at any time "
		"should there be a problem with the new version.\n\n"
		"Updated configuration bundles:"
	)));
	text->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
	content_sizer->Add(text);
	content_sizer->AddSpacer(VERT_SPACING);

	const auto lang_code = wxGetApp().current_language_code_safe().ToStdString();

	auto *versions = new wxBoxSizer(wxVERTICAL);
	for (const auto &update : updates) {
		auto *flex = new wxFlexGridSizer(2, 0, VERT_SPACING);

		auto *text_vendor = new wxStaticText(this, wxID_ANY, update.vendor);
		text_vendor->SetFont(boldfont);
		flex->Add(text_vendor);
		flex->Add(new wxStaticText(this, wxID_ANY, update.version.to_string()));

		if (! update.comment.empty()) {
			flex->Add(new wxStaticText(this, wxID_ANY, _(L("Comment:"))), 0, wxALIGN_RIGHT);
			auto *update_comment = new wxStaticText(this, wxID_ANY, from_u8(update.comment));
			update_comment->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
			flex->Add(update_comment);
		}

		if (! update.new_printers.empty()) {
			flex->Add(new wxStaticText(this, wxID_ANY, _L_PLURAL("New printer", "New printers", update.new_printers.find(',') == std::string::npos ? 1 : 2) + ":"), 0, wxALIGN_RIGHT);
			auto* update_printer = new wxStaticText(this, wxID_ANY, from_u8(update.new_printers));
			update_printer->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
			flex->Add(update_printer);
		}

		versions->Add(flex);

		if (! update.changelog_url.empty() && update.version.prerelease() == nullptr) {
			auto *line = new wxBoxSizer(wxHORIZONTAL);
			auto changelog_url = (boost::format(update.changelog_url) % lang_code).str();
			line->AddSpacer(3*VERT_SPACING);
			line->Add(new wxHyperlinkCtrl(this, wxID_ANY, _(L("Open changelog page")), changelog_url));
			versions->Add(line);
			versions->AddSpacer(1); // empty value for the correct alignment inside a GridSizer
		}
	}

	content_sizer->Add(versions);
	content_sizer->AddSpacer(2*VERT_SPACING);

	add_button(wxID_OK, true, force_before_wizard ? _L("Install") : "OK");
	if (force_before_wizard) {
		auto* btn = add_button(wxID_CLOSE, false, _L("Don't install"));
		btn->Bind(wxEVT_BUTTON, [this](const wxCommandEvent&) { this->EndModal(wxID_CLOSE); });
	}
	add_button(wxID_CANCEL);

	finalize();
}

MsgUpdateConfig::~MsgUpdateConfig() {}

//MsgUpdateForced

MsgUpdateForced::MsgUpdateForced(const std::vector<Update>& updates) :
    MsgDialog(nullptr, wxString::Format(_(L("%s incompatibility")), SLIC3R_APP_NAME), _(L("You must install a configuration update.")) + " ", wxOK | wxICON_ERROR)
{
	auto* text = new wxStaticText(this, wxID_ANY, wxString::Format(_(L(
		"%s will now start updates. Otherwise these profiles may have some settings modified after loading, and they may not work as expected.\n\n"
		"Note that a full configuration snapshot will be created first. It can then be restored at any time "
		"should there be a problem with the new version.\n\n"
		"Updated configuration bundles:"
	)), SLIC3R_APP_NAME));
	

	text->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
	content_sizer->Add(text);
	content_sizer->AddSpacer(VERT_SPACING);

	const auto lang_code = wxGetApp().current_language_code_safe().ToStdString();

	auto* versions = new wxFlexGridSizer(2, 0, VERT_SPACING);
	for (const auto& update : updates) {
		auto* text_vendor = new wxStaticText(this, wxID_ANY, update.vendor);
		text_vendor->SetFont(boldfont);
		versions->Add(text_vendor);
		versions->Add(new wxStaticText(this, wxID_ANY, update.version.to_string()));

		if (!update.comment.empty()) {
			versions->Add(new wxStaticText(this, wxID_ANY, _(L("Comment:")))/*, 0, wxALIGN_RIGHT*/);//uncoment if align to right (might not look good if 1  vedor name is longer than other names)
			auto* update_comment = new wxStaticText(this, wxID_ANY, from_u8(update.comment));
			update_comment->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
			versions->Add(update_comment);
		}

		if (!update.new_printers.empty()) {
			versions->Add(new wxStaticText(this, wxID_ANY, _L_PLURAL("New printer", "New printers", update.new_printers.find(',') == std::string::npos ? 1 : 2)+":")/*, 0, wxALIGN_RIGHT*/);
			auto* update_printer = new wxStaticText(this, wxID_ANY, from_u8(update.new_printers));
			update_printer->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
			versions->Add(update_printer);
		}

		if (!update.changelog_url.empty() && update.version.prerelease() == nullptr) {
			auto* line = new wxBoxSizer(wxHORIZONTAL);
			auto changelog_url = (boost::format(update.changelog_url) % lang_code).str();
			line->AddSpacer(3 * VERT_SPACING);
			line->Add(new wxHyperlinkCtrl(this, wxID_ANY, _(L("Open changelog page")), changelog_url));
			versions->Add(line);
			versions->AddSpacer(1); // empty value for the correct alignment inside a GridSizer
		}
	}

	content_sizer->Add(versions);
	content_sizer->AddSpacer(2 * VERT_SPACING);

    if (updates.size() > 1) {
        add_button(wxID_EDIT , false, _L("Choose which one to install"));
    }
    add_button(wxID_NO, false, wxString::Format(_L("Don't install")));
    add_button(wxID_EXIT, false, wxString::Format(_L("Exit %s"), SLIC3R_APP_NAME));

    get_button(wxID_EXIT)->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &evt) { this->EndModal(evt.GetId()); });
    get_button(wxID_NO)->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &evt) { this->EndModal(evt.GetId()); });
    if (updates.size() > 1) {
        get_button(wxID_EDIT)->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &evt) { this->EndModal(evt.GetId()); });
    }
    get_button(wxID_OK)->Bind(wxEVT_BUTTON, [this](const wxCommandEvent &evt) { this->EndModal(evt.GetId()); });

    finalize();
}

MsgUpdateForced::~MsgUpdateForced() {}

// MsgDataIncompatible

MsgDataIncompatible::MsgDataIncompatible(const std::unordered_map<std::string, wxString> &incompats) :
    MsgDialog(nullptr, wxString::Format(_(L("%s incompatibility")), SLIC3R_APP_NAME), 
                       wxString::Format(_(L("%s configuration is incompatible")), SLIC3R_APP_NAME), wxICON_ERROR)
{
	auto *text = new wxStaticText(this, wxID_ANY, wxString::Format(_(L(
		"This version of %s is not compatible with currently installed configuration bundles.\n"
		"This probably happened as a result of running an older %s after using a newer one.\n\n"

		"You may either exit %s and try again with a newer version, or you may re-run the initial configuration. "
		"Doing so will create a backup snapshot of the existing configuration before installing files compatible with this %s.")) + "\n", 
		SLIC3R_APP_NAME, SLIC3R_APP_NAME, SLIC3R_APP_NAME, SLIC3R_APP_NAME));
	text->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
	content_sizer->Add(text);

	auto *text2 = new wxStaticText(this, wxID_ANY, wxString::Format(_(L("This %s version: %s")), SLIC3R_APP_NAME, SLIC3R_VERSION_FULL));
	text2->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
	content_sizer->Add(text2);
	content_sizer->AddSpacer(VERT_SPACING);

	auto *text3 = new wxStaticText(this, wxID_ANY, _(L("Incompatible bundles:")));
	text3->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
	content_sizer->Add(text3);
	content_sizer->AddSpacer(VERT_SPACING);

	auto *versions = new wxFlexGridSizer(2, 0, VERT_SPACING);
	for (const auto &incompat : incompats) {
		auto *text_vendor = new wxStaticText(this, wxID_ANY, incompat.first);
		text_vendor->SetFont(boldfont);
		versions->Add(text_vendor);
		versions->Add(new wxStaticText(this, wxID_ANY, incompat.second));
	}

	content_sizer->Add(versions);
	content_sizer->AddSpacer(2*VERT_SPACING);

	add_button(wxID_REPLACE, true, _L("Re-configure"));
	add_button(wxID_EXIT, false, wxString::Format(_L("Exit %s"), SLIC3R_APP_NAME));

	for (auto ID : {wxID_EXIT, wxID_REPLACE})
		get_button(ID)->Bind(wxEVT_BUTTON, [this](const wxCommandEvent& evt) { this->EndModal(evt.GetId()); });

	finalize();
}

MsgDataIncompatible::~MsgDataIncompatible() {}


// MsgDataLegacy

MsgDataLegacy::MsgDataLegacy() :
	MsgDialog(nullptr, _(L("Configuration update")), _(L("Configuration update")))
{
    auto *text = new wxStaticText(this, wxID_ANY, format_wxstr( _L(
			"%s now uses an updated configuration structure.\n\n"

			"So called 'System presets' have been introduced, which hold the built-in default settings for various "
			"printers. These System presets cannot be modified, instead, users now may create their "
			"own presets inheriting settings from one of the System presets.\n"
			"An inheriting preset may either inherit a particular value from its parent or override it with a customized value.\n\n"

			"Please proceed with the %s that follows to set up the new presets "
			"and to choose whether to enable automatic preset updates."
        )
        , SLIC3R_APP_NAME, ConfigWizard::name()));
	text->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
	content_sizer->Add(text);
	content_sizer->AddSpacer(VERT_SPACING);

	auto *text2 = new wxStaticText(this, wxID_ANY, _(L("For more information please visit Prusa wiki page:")));
	// The wiki page name is intentionally not localized:
	// TRN %s = PrusaSlicer
	auto *link = new wxHyperlinkCtrl(this, wxID_ANY, format_wxstr(_L("%s 1.40 configuration update"), SLIC3R_APP_NAME), CONFIG_UPDATE_WIKI_URL);
	content_sizer->Add(text2);
	content_sizer->Add(link);
	content_sizer->AddSpacer(VERT_SPACING);

	finalize();
}

MsgDataLegacy::~MsgDataLegacy() {}


// MsgNoUpdate

MsgNoUpdates::MsgNoUpdates() :
    MsgDialog(nullptr, _(L("Configuration updates")), _(L("No updates available")), wxICON_ERROR | wxOK)
{

	auto* text = new wxStaticText(this, wxID_ANY, wxString::Format(
		_(L(
            "%s has no configuration updates available."
		)),
        SLIC3R_APP_NAME
	));
	text->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
	content_sizer->Add(text);
	content_sizer->AddSpacer(VERT_SPACING);

	finalize();
}

MsgNoUpdates::~MsgNoUpdates() {}

// MsgNoAppUpdates
MsgNoAppUpdates::MsgNoAppUpdates() :
	MsgDialog(nullptr, _(L("App update")), _(L("No updates available")), wxICON_ERROR | wxOK)
{
	//TRN %1% is PrusaSlicer
	auto* text = new wxStaticText(this, wxID_ANY, format_wxstr(_L("Your %1% is up to date."),SLIC3R_APP_NAME));
	text->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
	content_sizer->Add(text);
	content_sizer->AddSpacer(VERT_SPACING);

	finalize();
}

MsgNoAppUpdates::~MsgNoAppUpdates() {}

////// UpdateConfigDialog //////

// use event to call rebuild_ui to be sure to be in the right thread
wxDEFINE_EVENT(EVT_CONFIG_UPDATER_ERROR_MSG, wxCommandEvent);
wxDEFINE_EVENT(EVT_CONFIG_UPDATER_REDRAW, wxCommandEvent);
wxDEFINE_EVENT(EVT_VENDOR_VERSION_LAUNCH, wxCommandEvent);

void UpdateConfigDialog::request_rebuild_ui() {
    wxCommandEvent *evt = new wxCommandEvent(EVT_CONFIG_UPDATER_REDRAW);
    this->QueueEvent(evt);
}

void UpdateConfigDialog::request_rebuild_after_vendor_change(bool change_succeeded)
{
    if (!change_succeeded) {
        request_rebuild_ui();
        return;
    }

    // The preset reload starts its own repository refresh. Joining it here
    // guarantees that this dialog receives the terminal callback as well.
    m_data.sync_async(repository_operation_callback<int>(
        *this, [](UpdateConfigDialog &dialog, int) { dialog.request_rebuild_ui(); }));
}

void UpdateConfigDialog::request_show_error_msg(const std::string &error_msg) {
    if (!error_msg.empty()) {
        wxCommandEvent *evt = new wxCommandEvent(EVT_CONFIG_UPDATER_ERROR_MSG);
        evt->SetString(error_msg);
        this->QueueEvent(evt);
    }
}

void UpdateConfigDialog::begin_vendor_operation(const wxString &message)
{
    begin_repository_operation(nullptr, message);
    apply_vendor_operation_state();
}

void UpdateConfigDialog::finish_vendor_operation()
{
    finish_repository_operation(nullptr);
    apply_vendor_operation_state();
}

void UpdateConfigDialog::apply_vendor_operation_state()
{
    const bool enable_actions = !repository_operation_in_progress();
    for (wxWindow *control : m_repository_action_controls)
        control->Enable(enable_actions);
}

void UpdateConfigDialog::add_vendor_in_list(wxWindow *parent, VendorSync &vendor, wxGridBagSizer *versions_sizer, const int line_num) {
    const std::string vendor_id = vendor.profile.id;
    const VendorAvailable *best = vendor.best_available();
    const bool has_compatible_version = best != nullptr;
    const VendorAvailable best_version = best != nullptr ? *best : VendorAvailable{};
    ////// name //////
    wxStaticText *msg_name = new wxStaticText(parent, wxID_ANY, vendor.profile.full_name);
    msg_name->SetToolTip(vendor.profile.description);
    versions_sizer->Add(msg_name, wxGBPosition(line_num, 1), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);

    ////// version selector button //////
    wxString bt_version_msg;
    if (vendor.is_installed) {
        assert(vendor.profile.config_version != Semver::zero());
        bt_version_msg = vendor.profile.config_version.to_string();
    } else if (!vendor.available_profiles.empty()) {
        bt_version_msg = _L("Choose version");
    } else {
        bt_version_msg = _L("Not installed");
    }
    // The active panel remains able to receive mouse events when its native
    // button is disabled, which lets Windows display the explanatory tooltip.
    wxPanel *bt_version_panel = new wxPanel(parent, wxID_ANY);
    wxBoxSizer *bt_version_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton *bt_version = new wxButton(bt_version_panel, wxID_ANY, bt_version_msg);
    bt_version_sizer->Add(bt_version, 1, wxEXPAND);
    bt_version_panel->SetSizer(bt_version_sizer);
    if (vendor.available_profiles.empty()) {
        bt_version->Enable(false);
        bt_version_panel->SetToolTip(
            _L("No vendor profile version is available in the local cache or repository."));
    } else {
        bt_version->Bind(wxEVT_BUTTON, ([this, vendor_id](wxCommandEvent &e) {
            m_data.download_changelogs(
                vendor_id,
                repository_operation_callback<bool>(
                    *this, [vendor_id](UpdateConfigDialog &dialog, bool) {
                        wxCommandEvent *evt = new wxCommandEvent(EVT_VENDOR_VERSION_LAUNCH);
                        evt->SetString(vendor_id);
                        dialog.QueueEvent(evt);
                    }));
        }));
        bt_version->SetToolTip(_L("Click this button to choose a vendor bundle version to install."));
    }
    versions_sizer->Add(bt_version_panel, wxGBPosition(line_num, 2), wxGBSpan(1, 1), wxEXPAND, 2);

    ////// upgrade //////
    wxStaticText *msg_synch = nullptr;
    if (!vendor.is_installed && has_compatible_version && !best->local_file.empty()) {
        // A manually imported or bundled profile is immediately installable.
        // Repository synchronization must not hide a complete local package.
        wxString config_version_str = best->config_version.to_string();
        wxString msg = format(_L("Install %1% (local)"), config_version_str);
        wxButton *bt_upgrade = new wxButton(parent, wxID_ANY, msg);
        bt_upgrade->SetToolTip(
            _L("Click this button to create a snapshot and install the compatible vendor bundle already available "
               "in the local cache."));
        versions_sizer->Add(bt_upgrade, wxGBPosition(line_num, 3), wxGBSpan(1, 1), wxEXPAND, 2);
        bt_upgrade->Bind(wxEVT_BUTTON, ([this, vendor_id, best_version](wxCommandEvent &e) {
            this->begin_vendor_operation(_L("Installing the local preset, please wait"));
            this->m_data.install_vendor(
                vendor_id, best_version,
                repository_operation_callback<std::string>(
                    *this, [](UpdateConfigDialog &dialog, std::string error_msg) {
                        dialog.finish_vendor_operation();
                        dialog.request_show_error_msg(error_msg);
                        dialog.request_rebuild_ui();
                    }));
        }));
    } else if (vendor.profile.config_update_rest.empty()) {
        if (vendor.can_upgrade && has_compatible_version) {
            assert(best->config_version > vendor.profile.config_version);
            wxString config_version_str = best->config_version.to_string();
            wxString msg = vendor.is_installed ? format(_L("Upgrade to %1%"), config_version_str) :
                                                 format(_L("Install %1%"), config_version_str);
            wxButton *bt_upgrade = new wxButton(parent, wxID_ANY, msg);
            if (vendor.is_installed) {
                bts_green_color.push_back(bt_upgrade);
            }
            bt_upgrade->SetToolTip(_L("Click this button to create a snapshot and upgrade this vendor bundle to the "
                                      "latest compatible version."));
            versions_sizer->Add(bt_upgrade, wxGBPosition(line_num, 3), wxGBSpan(1, 1), wxEXPAND, 2);
            bt_upgrade->Bind(wxEVT_BUTTON, ([this, vendor_id, best_version](wxCommandEvent &e) {
                                 this->begin_vendor_operation(_L("Upgrading the preset, please wait"));
                                 this->m_data.install_vendor(
                                     vendor_id, best_version,
                                     repository_operation_callback<std::string>(
                                         *this, [](UpdateConfigDialog &dialog, std::string error_msg) {
                                             dialog.finish_vendor_operation();
                                             dialog.request_show_error_msg(error_msg);
                                             dialog.request_rebuild_ui();
                                         }));
                             }));
        } else if (!vendor.available_profiles.empty() && !has_compatible_version) {
            msg_synch = new wxStaticText(parent, wxID_ANY, _L("No compatible profile"));
            msg_synch->SetToolTip(
                _L("The local cache contains vendor profiles, but none of them target this slicer version."));
        } else {
            msg_synch = new wxStaticText(parent, wxID_ANY, _L("Local bundle"));
            msg_synch->SetToolTip(
                _L("This printer vendor bundle doesn't have a repository and therefore cannot be updated. "
                   "\nA future slicer version may include an updated static profile."));
        }
    } else if (vendor.sync_state == RepositorySyncState::Succeeded) {
        if (vendor.available_profiles.empty()) {
            // weird
            msg_synch = new wxStaticText(parent, wxID_ANY, _L("No profile available"));
            msg_synch->SetToolTip(_L("This printer vendor doesn't have any available presets for this slicer."));
        } else if (!has_compatible_version) {
            msg_synch = new wxStaticText(parent, wxID_ANY, _L("No compatible profile"));
            msg_synch->SetToolTip(
                _L("The repository contains vendor profiles, but none of them target this slicer version."));
        } else if (!vendor.is_installed || vendor.can_upgrade) {
            assert(!vendor.is_installed || best->config_version > vendor.profile.config_version);
            wxString config_version_str = best->config_version.to_string();
            wxString msg = vendor.is_installed ? format(_L("Upgrade to %1%"), config_version_str) :
                                                 format(_L("Install %1%"), config_version_str);
            wxButton *bt_upgrade = new wxButton(parent, wxID_ANY, msg);
            if (vendor.is_installed) {
                bts_green_color.push_back(bt_upgrade);
            }
            bt_upgrade->SetToolTip(_L("Click this button to create a snapshot and upgrade this vendor bundle to the "
                                      "latest compatible version."));
            versions_sizer->Add(bt_upgrade, wxGBPosition(line_num, 3), wxGBSpan(1, 1), wxEXPAND, 2);
            bt_upgrade->Bind(wxEVT_BUTTON, ([this, vendor_id, best_version](wxCommandEvent &e) {
                this->begin_vendor_operation(_L("Upgrading the preset, please wait"));
                this->m_data.install_vendor(
                    vendor_id, best_version,
                    repository_operation_callback<std::string>(
                        *this, [](UpdateConfigDialog &dialog, std::string error_msg) {
                            dialog.finish_vendor_operation();
                            dialog.request_show_error_msg(error_msg);
                            dialog.request_rebuild_ui();
                        }));
            }));
        } else {
            msg_synch = new wxStaticText(parent, wxID_ANY, _L("Up to date"));
        }
    } else if (vendor.sync_state == RepositorySyncState::InProgress) {
        msg_synch = new wxStaticText(parent, wxID_ANY, _L("Synch with github ..."));
    } else if (vendor.sync_state == RepositorySyncState::Failed) {
        assert(!vendor.sync_error.succeeded());
        if (has_compatible_version && !vendor.is_installed) {
            wxString config_version_str = best->config_version.to_string();
            wxString msg = format(_L("Install %1% from cache"), config_version_str);
            wxButton *bt_upgrade = new wxButton(parent, wxID_ANY, msg);
            if (vendor.is_installed) {
                bts_green_color.push_back(bt_upgrade);
            }
            wxString tooltip = from_u8(format_updater_error(vendor.sync_error));
            tooltip += "\n\n";
            tooltip += _L("A compatible vendor bundle remains available in the local cache. Click this button to "
                          "create a snapshot and install it.");
            bt_upgrade->SetToolTip(tooltip);
            versions_sizer->Add(bt_upgrade, wxGBPosition(line_num, 3), wxGBSpan(1, 1), wxEXPAND, 2);
            bt_upgrade->Bind(wxEVT_BUTTON, ([this, vendor_id, best_version](wxCommandEvent &e) {
                this->begin_vendor_operation(_L("Installing the preset, please wait"));
                this->m_data.install_vendor(
                    vendor_id, best_version,
                    repository_operation_callback<std::string>(
                        *this, [](UpdateConfigDialog &dialog, std::string error_msg) {
                            dialog.finish_vendor_operation();
                            dialog.request_show_error_msg(error_msg);
                            dialog.request_rebuild_ui();
                        }));
            }));
        } else {
            wxString label = from_u8(updater_error_short_label(vendor.sync_error));
            if (label.empty())
                label = _L("Synchronization failed");
            msg_synch = new wxStaticText(parent, wxID_ANY, label);

            wxString tooltip = from_u8(format_updater_error(vendor.sync_error));
            if (!vendor.available_profiles.empty() && !has_compatible_version) {
                tooltip += "\n\n";
                tooltip += _L("The local cache also contains no vendor profile compatible with this slicer version.");
            }
            msg_synch->SetToolTip(tooltip);
        }
    } else {
        msg_synch = new wxStaticText(parent, wxID_ANY, _L("Unchecked"));
        msg_synch->SetToolTip(_L("This printer vendor bundle may have a new preset available online. Click the "
                                 "'Check for updates' button to check."));
    }
    if (msg_synch) {
        versions_sizer->Add(msg_synch, wxGBPosition(line_num, 3), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);
    }

    ////// Uninstall button //////
    wxString bt_uninstall_msg;
    if (vendor.is_installed) {
        assert(vendor.profile.config_version != Semver::zero());
        bt_uninstall_msg = _L("Uninstall");
    } else if (vendor.has_cache) {
        bt_uninstall_msg = _L("Clear cache");
    } else {
        bt_uninstall_msg = _L("Not installed");
    }
    wxButton *bt_uninstall = new wxButton(parent, wxID_ANY, bt_uninstall_msg);
    if (!vendor.is_installed && !vendor.has_cache) {
        bt_uninstall->Enable(false);
    } else {
        bt_uninstall->Bind(wxEVT_BUTTON, ([this, vendor_id, vendor_is_installed = vendor.is_installed, vendor_has_cache = vendor.has_cache, vendor_full_name = vendor.profile.full_name](wxCommandEvent &e) {
                if (vendor_is_installed) {
                    bool apply_keeped_changes_useless;
                    if (!wxGetApp().check_and_keep_current_preset_changes(_L("Uninstalling a vendor bundle"),
                                                                          _L("Uninstalling a vendor bundle"),
                                                                          ActionButtons::SAVE,
                                                                          &apply_keeped_changes_useless)) {
                        return;
                    }
                    MessageDialog msg_dlg(this,
                                          format(_L("Are you sure you want to uninstall this vendor bundle:\n%1%?"),
                                                 vendor_full_name),
                                          _L("Uninstall vendor bundle"), wxICON_WARNING | wxOK | wxCANCEL);
                    if (msg_dlg.ShowModal() == wxID_OK) {
                        this->m_data.uninstall_vendor(
                            vendor_id,
                            repository_operation_callback<bool>(
                                *this, [](UpdateConfigDialog &dialog, bool success) {
                                    dialog.request_rebuild_after_vendor_change(success);
                                }));
                    }
                } else if (vendor_has_cache) {
                    MessageDialog msg_dlg(this,
                                          format(_L("Are you sure you want to remove all cache files about this vendor bundle :\n%1%?"),
                                                 vendor_full_name),
                                          _L("Clear vendor bundle cache"), wxICON_WARNING | wxOK | wxCANCEL);
                    if (msg_dlg.ShowModal() == wxID_OK) {
                        this->m_data.clear_cache_vendor(
                            vendor_id,
                            repository_operation_callback<bool>(
                                *this, [](UpdateConfigDialog &dialog, bool success) {
                                    dialog.request_rebuild_after_vendor_change(success);
                                }));
                    }
                }
        }));
    }
    bt_uninstall->SetToolTip(_L(
        "Click the button to uninstall the vendor bundle. The printer(s) will no longer be selectable in the wizard or the slicer. "
        "\nThe uninstalled bundle will be saved in a cache so you can reinstall it at any time. "
        "\nNote: If a user profile depends on a profile from this bundle, it may break. Please detach them beforehand."));
    versions_sizer->Add(bt_uninstall, wxGBPosition(line_num, 4), wxGBSpan(1, 1), wxEXPAND, 2);
}

void UpdateConfigDialog::build_ui() {
    bts_green_color.clear();
    m_repository_action_controls.clear();

    if (!m_message.empty()) {
        wxStaticText *lbl_message = new wxStaticText(this, wxID_ANY, m_message);
        lbl_message->SetToolTip(
            "This dialog allows you to manage the different preset bundles that can be installed for this slicer."
            "\nYou need to install a bundle (this copies the bundle files into your configuration) so the wizard can access it, allowing it to appear in the slicer's user interface."
            "\nA snapshot is created before any installation or removal, so you can always revert to a previous state if you make a mistake."
            "\nYou can choose a specific version by clicking the button in the second column of an installed bundle."
            "\nTo add new bundles that are not yet known to the slicer, you can either provide the GitHub repository URL or load a vendor .ini file (these start with a [vendor] section)."
            "\nYou can install all vendor bundles, but doing so may significantly increase the time it takes to open the wizard."
        );
        wxBoxSizer *message_sizer = new wxBoxSizer(wxHORIZONTAL);
        message_sizer->AddSpacer(5);
        message_sizer->Add(lbl_message);
        main_sizer->AddSpacer(5);
        main_sizer->Add(message_sizer);
    }

    // button to synch
    wxButton* bt_synch = new wxButton(this, wxID_ANY, _L("Force check for updates"));
    bt_synch->Bind(wxEVT_BUTTON, ([this](wxCommandEvent &e) {
        this->begin_vendor_operation(_L("Updating the presets, please wait"));
        this->m_data.reload_all_vendors();
        this->m_data.sync_async(
            repository_operation_callback<int>(
                *this, [](UpdateConfigDialog &dialog, int) {
                    dialog.finish_vendor_operation();
                    dialog.request_rebuild_ui();
                }),
            true);
    }));
    wxBoxSizer *bt_synch_sizer = new wxBoxSizer(wxHORIZONTAL);
    bt_synch_sizer->AddSpacer(5);
    bt_synch_sizer->Add(bt_synch);
    m_repository_action_controls.emplace_back(bt_synch);
    main_sizer->AddSpacer(10);
    main_sizer->Add(bt_synch_sizer);

    // field to add a new repository
    txt_new_repo = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(350,30));
    txt_new_repo->SetHint("github.com/SuperSlicer_org/Basic");
    // button to add a new repository
    wxButton *bt_add = new wxButton(this, wxID_ANY, _L("Add a new vendor"));
    bt_add->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) {
        std::string rest_url = txt_new_repo->GetValue().utf8_string();
        rest_url = VendorProfile::get_http_url_rest(rest_url);
        this->m_data.download_new_repo(
            rest_url,
            repository_operation_callback<bool>(
                *this, [rest_url](UpdateConfigDialog &dialog, bool result) {
                    if (result) {
                        dialog.m_data.reload_all_vendors();
                        dialog.m_data.sync_async(dialog.repository_operation_callback<int>(
                            dialog, [](UpdateConfigDialog &alive_dialog, int) {
                                alive_dialog.request_rebuild_ui();
                            }));
                    } else if (rest_url.find("https://api.github.com/repos/") != std::string::npos) {
                        const std::string org_repo_part =
                            rest_url.substr(strlen("https://api.github.com/repos/"));
                        MessageDialog msg_dlg(
                            &dialog,
                            format(_L("Failed to read this vendor bundle at the url '%1%': the url is malformed, "
                                      "or the repository doesn't have a correct description.ini\n"),
                                   std::string("https://raw.githubusercontent.com/") + org_repo_part +
                                       "/HEAD/description.ini"),
                            _L("Fail to add a new vendor bundle"),
                            wxICON_WARNING | wxOK);
                        msg_dlg.ShowModal();
                    } else {
                        MessageDialog msg_dlg(
                            &dialog,
                            format(_L("Failed to read this vendor bundle at the url '%1%': the url is malformed, "
                                      "the repository doesn't have a correct description.ini\n"),
                                   rest_url + "/description"),
                            _L("Fail to add a new vendor bundle"),
                            wxICON_WARNING | wxOK);
                        msg_dlg.ShowModal();
                    }
                }));
    }));
    wxButton *bt_load_ini = new wxButton(this, wxID_ANY, _L("Load vendor ini file"));
    bt_load_ini->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxFileDialog dialog(this, _L("Load vendor configuration bundle"), "", "", "*.ini",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        wxGetApp().UpdateDarkUI(&dialog);
        if (dialog.ShowModal() != wxID_OK)
            return;

        // The updater validates the profile and atomically replaces an older
        // cached copy before rebuilding the dialog from the refreshed model.
        begin_vendor_operation(_L("Loading the vendor profile, please wait"));
        const boost::filesystem::path profile_path(into_path(dialog.GetPath()));
        m_data.cache_vendor_ini(
            profile_path,
            repository_operation_callback<std::string>(
                *this, [](UpdateConfigDialog &dialog, std::string error_msg) {
                    dialog.finish_vendor_operation();
                    dialog.request_show_error_msg(error_msg);
                    dialog.request_rebuild_ui();
                }));
    });
    wxButton *bt_load_archive = new wxButton(this, wxID_ANY, _L("Load vendor archive"));
    bt_load_archive->SetToolTip(_L("Load a ZIP vendor bundle into the local version cache."));
    bt_load_archive->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxFileDialog dialog(this, _L("Load vendor bundle archive"), "", "",
                            _L("Vendor bundle archives (*.zip)|*.zip"),
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        wxGetApp().UpdateDarkUI(&dialog);
        if (dialog.ShowModal() != wxID_OK)
            return;

        // Archive parsing and cache publication are owned by the core updater.
        // The GUI only keeps the user informed and rebuilds from the new model.
        begin_vendor_operation(_L("Loading the vendor archive, please wait"));
        const boost::filesystem::path archive_path(into_path(dialog.GetPath()));
        m_data.cache_vendor_archive(
            archive_path,
            repository_operation_callback<std::string>(
                *this, [](UpdateConfigDialog &dialog, std::string error_msg) {
                    dialog.finish_vendor_operation();
                    dialog.request_show_error_msg(error_msg);
                    dialog.request_rebuild_ui();
                }));
    });
    wxBoxSizer *github_add_sizer = new wxBoxSizer(wxHORIZONTAL);
    github_add_sizer->AddSpacer(5);
    github_add_sizer->Add(txt_new_repo);
    github_add_sizer->AddSpacer(5);
    github_add_sizer->Add(bt_add);
    github_add_sizer->AddSpacer(5);
    github_add_sizer->Add(bt_load_ini);
    github_add_sizer->AddSpacer(5);
    github_add_sizer->Add(bt_load_archive);
    github_add_sizer->AddSpacer(15);
    main_sizer->AddSpacer(5);
    main_sizer->Add(github_add_sizer);
    m_repository_action_controls.emplace_back(txt_new_repo);
    m_repository_action_controls.emplace_back(bt_add);
    m_repository_action_controls.emplace_back(bt_load_ini);
    m_repository_action_controls.emplace_back(bt_load_archive);

    // "table" of all availables repositories
    wxGridBagSizer *versions_sizer = new wxGridBagSizer(10, 30); //(int vgap, int hgap)
    versions_sizer->AddGrowableCol(1);
    hscroll = new wxScrolledWindow(this);

    // each row has:
    // [name] [synch status (with color)] [installed version]
    // name has the description as a tooltip
    // installed version is a button to pop a dialog to select the desired version to install.

    int row_idx = 1;
    wxStaticText *msg_name = new wxStaticText(hscroll, wxID_ANY, _L("Vendor Name"));
    versions_sizer->Add(msg_name, wxGBPosition(row_idx, 1), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);
    wxStaticText *msg_version = new wxStaticText(hscroll, wxID_ANY, _L("Installed version"));
    versions_sizer->Add(msg_version, wxGBPosition(row_idx, 2), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);
    wxStaticText *msg_upgrade = new wxStaticText(hscroll, wxID_ANY, _L("Upgrade"));
    versions_sizer->Add(msg_upgrade, wxGBPosition(row_idx, 3), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);
    wxStaticText *msg_uninstall = new wxStaticText(hscroll, wxID_ANY, _L("Uninstall"));
    versions_sizer->Add(msg_uninstall, wxGBPosition(row_idx, 4), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);

    ++row_idx;
    wxButton *bt_install_all = new wxButton(hscroll, wxID_ANY, _L("Install all"));
    versions_sizer->Add(bt_install_all, wxGBPosition(row_idx, 2), wxGBSpan(1, 1), wxEXPAND, 2);
    wxButton *bt_upgrade_all = new wxButton(hscroll, wxID_ANY, _L("Upgrade all"));
    versions_sizer->Add(bt_upgrade_all, wxGBPosition(row_idx, 3), wxGBSpan(1, 1), wxEXPAND, 2);
    wxButton *bt_uninstall_all = new wxButton(hscroll, wxID_ANY, _L("Uninstall all"));
    versions_sizer->Add(bt_uninstall_all, wxGBPosition(row_idx, 4), wxGBSpan(1, 1), wxEXPAND, 2);
    bt_install_all->Bind(wxEVT_BUTTON, ([this](wxCommandEvent &e) {
        this->begin_vendor_operation(_L("Installing the presets, please wait"));
        this->m_data.install_all_vendors(repository_operation_callback<std::string>(
            *this, [](UpdateConfigDialog &dialog, std::string error_msg) {
                dialog.finish_vendor_operation();
                dialog.request_show_error_msg(error_msg);
                dialog.request_rebuild_ui();
            }));
    }));
    bt_upgrade_all->Bind(wxEVT_BUTTON, ([this](wxCommandEvent &e) {
        this->begin_vendor_operation(_L("Updating the presets, please wait"));
        this->m_data.upgrade_all_installed_vendors(repository_operation_callback<std::string>(
            *this, [](UpdateConfigDialog &dialog, std::string error_msg) {
                dialog.finish_vendor_operation();
                dialog.request_show_error_msg(error_msg);
                dialog.request_rebuild_ui();
            }));
    }));
    bt_uninstall_all->Bind(wxEVT_BUTTON, ([this](wxCommandEvent &e) {
        MessageDialog msg_dlg(this,
                _L("Are you sure to uninstall all the vendor bundles ? \nThis will remove all the system "
                          "printers from the slicer. \nA snapshot will be made in case you want to revert."),
            _L("Uninstall vendor bundle"), wxICON_WARNING | wxOK |wxCANCEL);
        if (msg_dlg.ShowModal() == wxID_OK) {
            this->begin_vendor_operation(_L("Uninstalling the presets, please wait"));
            this->m_data.uninstall_all_vendors(repository_operation_callback<bool>(
                *this, [](UpdateConfigDialog &dialog, bool ok) {
                    dialog.finish_vendor_operation();
                    dialog.request_rebuild_after_vendor_change(ok);
                }));
        }
    }));

    std::vector<VendorSync> ordered_vendors = m_data.vendors();
    std::sort(ordered_vendors.begin(), ordered_vendors.end(), [](const VendorSync &left, const VendorSync &right) {
        if (left.is_installed == right.is_installed) {
            if (left.can_upgrade == right.can_upgrade) {
                return left.profile.name < right.profile.name;
            } else {
                return left.can_upgrade;
            }
        } else {
            return left.is_installed;
        }
    });
    for (VendorSync &vendor : ordered_vendors) {
        // Sorting moves VendorSync values. Rebuild best so it points to this
        // copied row's available_profiles rather than a previous container.
        vendor.sort_available();
        add_vendor_in_list(hscroll, vendor, versions_sizer, ++row_idx);
    }

    //if no bundle, then deactivate the button and add a text.
    if (m_data.count_available() == 0) {
        bt_install_all->Enable(false);
        bt_upgrade_all->Enable(false);
        bt_uninstall_all->Enable(false);
        ++row_idx;
        wxStaticText *msg_name = new wxStaticText(hscroll, wxID_ANY, _L("No vendor bundle available. Please add one manually or by adding a repository."));
        msg_name->SetToolTip(_L("There is no vendor bundle included with this version of the slicer. You can load a vendor INI file or ZIP archive, "
            "or add a vendor repository (for example, SuperSlicer-org/Voron-Profile) by entering the URL in the text field and then clicking the 'Add' button."));
        versions_sizer->Add(msg_name, wxGBPosition(row_idx, 1), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);
    }

    // scrollling: only vertical, by 30 pixels at a time.
    hscroll->SetScrollRate(30, 30);
    hscroll->EnableScrolling(false, true); // does nothing
    //hscroll->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT wxSHOW_SB_ALWAYS);
    hscroll->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
    wxBoxSizer *hscrollsizer = new wxBoxSizer(wxHORIZONTAL);
    hscrollsizer->AddSpacer(5);
    hscrollsizer->Add(versions_sizer);
    hscrollsizer->AddSpacer(5);
    hscroll->SetSizer(hscrollsizer);
    hscrollsizer->Layout();

    hscrollsizer->FitInside(hscroll);
    hscroll->SetVirtualSize(wxSize(hscroll->GetVirtualSize().GetWidth()+15, hscroll->GetVirtualSize().GetHeight()));
    hscroll->SetMinSize(wxSize(hscroll->GetVirtualSize().GetWidth(), std::min(500, hscroll->GetVirtualSize().GetHeight())));
    m_repository_action_controls.emplace_back(hscroll);
    apply_vendor_operation_state();
    main_sizer->Add(hscroll, 1, wxEXPAND | wxALL);

    wxStdDialogButtonSizer* btSizer = CreateStdDialogButtonSizer(wxOK);
    main_sizer->Add(btSizer, 0, wxALL);
    main_sizer->AddSpacer(5);
}

UpdateConfigDialog::UpdateConfigDialog(wxWindow *parent, PresetUpdater &data, const wxString &message)
    : RepositoryUpdatesDialogBase(parent, _L("Configuration manager"))
    , m_data(data)
    , m_message(message)
{

    this->Bind(EVT_CONFIG_UPDATER_REDRAW, [this](const wxCommandEvent& evt) {
        this->rebuild_ui();
    });
    this->Bind(EVT_CONFIG_UPDATER_ERROR_MSG, [this](const wxCommandEvent& evt) {
        std::string error_msg = evt.GetString().ToStdString();
        MessageDialog msg_dlg(this,error_msg, _L("Error"), wxICON_ERROR | wxOK);
        msg_dlg.ShowModal();
    });
    this->Bind(EVT_VENDOR_VERSION_LAUNCH, [this](const wxCommandEvent& evt) {
        // create new dialog/expand current to select choosen version.
        std::string vendor_id = evt.GetString().ToStdString();
        assert(!vendor_id.empty());
        const std::optional<VendorSync> vendor = m_data.vendor(vendor_id);
        assert(vendor.has_value());
        if (vendor.has_value()) {
            ChooseVendorVersionDialog choose_version(this, m_data, *vendor);
            choose_version.ShowModal();
            this->rebuild_ui();
        }
    });

    main_sizer = new wxBoxSizer(wxVERTICAL);
    build_ui();
    SetMaxSize(wxSize(std::max(400, int(parent->GetSize().GetWidth() * 0.8)),
                      std::max(600, int(parent->GetSize().GetHeight() * 0.8))));
    SetSizer(main_sizer);
    //SetSizeHints();
    main_sizer->SetSizeHints(this);
    FitInside();

    // add size for scroll bar and size for the bottom buttons
    // i don't know why it's need and I don't want to lost another day on it.
    SetMinSize(wxSize(GetMinSize().GetWidth() +20, GetMinSize().GetHeight() + 60));
    //set size if preferred not too big
    SetSize(GetMinSize().GetWidth(),
            std::min(GetMaxSize().GetHeight(),
                     std::max(GetBestVirtualSize().GetHeight(),
                              hscroll->GetBestVirtualSize().GetHeight() + 180)));

    wxGetApp().UpdateDlgDarkUI(this);
    wxColour pale_green(127,250,127);
    for (wxButton *bt : bts_green_color) {
        bt->SetBackgroundColour(pale_green);
    }
    this->CenterOnParent();
}

void UpdateConfigDialog::rebuild_ui() {
    Freeze();
    main_sizer->Clear(true);
    this->build_ui();
    SetSizer(main_sizer);
    //don't fit to not change size

    wxGetApp().UpdateDlgDarkUI(this);
    wxColour pale_green(127,250,127);
    for (wxButton *bt : bts_green_color) {
        bt->SetBackgroundColour(pale_green);
    }
    Layout();
    Thaw();
    Refresh();

}

// ChooseVendorVersionDialog //

//use event to call rebuild_ui to be sure to be in the right thread
wxDEFINE_EVENT(EVT_VENDOR_VERSION_ERROR_MSG, wxCommandEvent);
wxDEFINE_EVENT(EVT_VENDOR_VERSION_REDRAW, wxCommandEvent);

void ChooseVendorVersionDialog::request_rebuild_ui() {
    wxCommandEvent *evt = new wxCommandEvent(EVT_VENDOR_VERSION_REDRAW);
    this->QueueEvent(evt);
}

void ChooseVendorVersionDialog::request_show_error_msg(const std::string &error_msg) {
    if (!error_msg.empty()) {
        wxCommandEvent *evt = new wxCommandEvent(EVT_VENDOR_VERSION_ERROR_MSG);
        evt->SetString(error_msg);
        this->QueueEvent(evt);
    }
}

ChooseVendorVersionDialog::ChooseVendorVersionDialog(wxWindow *parent, PresetUpdater &data, const VendorSync &vendor)
    : RepositoryUpdatesDialogBase(
          parent, format(_L("Available versions for the %1% vendor bundle."), vendor.profile.full_name))
    , m_data(data)
    , m_vendor(vendor)
{

    this->Bind(EVT_VENDOR_VERSION_REDRAW, [this](const wxCommandEvent& evt) {
        this->rebuild_ui();
    });
    this->Bind(EVT_VENDOR_VERSION_ERROR_MSG, [this](const wxCommandEvent& evt) {
        std::string error_msg = evt.GetString().ToStdString();
        MessageDialog msg_dlg(this,error_msg, _L("Error"), wxICON_ERROR | wxOK);
        msg_dlg.ShowModal();
    });

    build_ui();
    //SetMaxSize(wxSize(std::max(400, int(parent->GetSize().GetWidth() * 0.8)),
                      //std::max(600, int(parent->GetSize().GetHeight() * 0.8))));
    SetSizer(main_sizer);
    //SetSizeHints();
    main_sizer->SetSizeHints(this);
    FitInside();

    //// add size for scroll bar ans size for the bottom buttons
    //// i don't know why it's need and I don't want to lost another day on it.
    SetMinSize(wxSize(GetMinSize().GetWidth() + 20, GetMinSize().GetHeight() + 60));
    ////set size if preferred not too big
    SetSize(GetMinSize().GetWidth(),
            std::min(GetMaxSize().GetHeight(),
                     std::max(GetBestVirtualSize().GetHeight(),
                              hscroll->GetBestVirtualSize().GetHeight() + 180)));

    wxGetApp().UpdateDlgDarkUI(this);
    wxColour pale_green(127,250,127);
    for (wxWindow *bt : green_foreground_color) {
        bt->SetBackgroundColour(pale_green);
    }
    wxColour pale_red(250,127,127);
    for (wxWindow *bt : red_foreground_color) {
        bt->SetBackgroundColour(pale_red);
    }
    this->CenterOnParent();
}

void ChooseVendorVersionDialog::rebuild_ui() {
    Freeze();
    main_sizer->Clear(true);
    const std::optional<VendorSync> vendor = m_data.vendor(this->m_vendor.profile.id);
    assert(vendor.has_value());
    if (vendor.has_value()) {
        this->m_vendor = *vendor;
    }
    this->build_ui();
    SetSizer(main_sizer);
    //don't fit to not change size

    wxGetApp().UpdateDlgDarkUI(this);
    wxColour pale_green(127,250,127);
    for (wxWindow *bt : green_foreground_color) {
        bt->SetBackgroundColour(pale_green);
    }
    wxColour pale_red(250,127,127);
    for (wxWindow *bt : red_foreground_color) {
        bt->SetBackgroundColour(pale_red);
    }
    Layout();
    Thaw();
    Refresh();

}

void ChooseVendorVersionDialog::build_ui() {
    green_foreground_color.clear();
    red_foreground_color.clear();
    main_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText *lbl_message = new wxStaticText(this, wxID_ANY,
                                                 _L("This dialog allows you to choose which version of the vendor "
                                                    "bundle you want to install from the available options."));
    main_sizer->AddSpacer(5);
    main_sizer->Add(lbl_message);

    // "table" of all availables versions
    wxGridBagSizer *versions_sizer = new wxGridBagSizer(10, 30); //(int vgap, int hgap)
    versions_sizer->AddGrowableCol(3);
    hscroll = new wxScrolledWindow(this);

    // each row has:
    // [version number] [slicer version] [changelog]

    int row_idx = 1;
    wxStaticText *msg_vendor_version = new wxStaticText(this, wxID_ANY, _L("Vendor bundle version"));
    versions_sizer->Add(msg_vendor_version, wxGBPosition(row_idx, 1), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);
    wxStaticText *msg_slicer_version = new wxStaticText(this, wxID_ANY, _L("Minimum slicer version"));
    msg_slicer_version->SetToolTip(format(_L("Our slicer version: %1%"), SLIC3R_VERSION_FULL));
    versions_sizer->Add(msg_slicer_version, wxGBPosition(row_idx, 2), wxGBSpan(1, 1), wxALIGN_RIGHT, 2);
    wxStaticText *msg_changelog = new wxStaticText(this, wxID_ANY, _L("Changelog"));
    versions_sizer->Add(msg_changelog, wxGBPosition(row_idx, 3), wxGBSpan(1, 1), wxALIGN_LEFT, 2);

    for (const VendorAvailable &available : m_vendor.available_profiles) {
        add_version_in_list(hscroll, available, versions_sizer, ++row_idx);
    }

    // scrollling: only vertical, by 30 pixels at a time.
    hscroll->SetScrollRate(30, 30);
    hscroll->EnableScrolling(false, true); // does nothing
    hscroll->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);

    hscroll->SetSizer(versions_sizer);
    versions_sizer->FitInside(hscroll);
    hscroll->SetVirtualSize(wxSize(hscroll->GetVirtualSize().GetWidth()+15, hscroll->GetVirtualSize().GetHeight()));
    hscroll->SetMinSize(wxSize(hscroll->GetVirtualSize().GetWidth(), std::min(500, hscroll->GetVirtualSize().GetHeight())));
    hscroll->Enable(!repository_operation_in_progress());
    main_sizer->Add(hscroll, 1, wxEXPAND | wxALL);

    wxStdDialogButtonSizer* btSizer = CreateStdDialogButtonSizer(wxOK);
    main_sizer->Add(btSizer, 0, wxALL);
    main_sizer->AddSpacer(5);
}

void ChooseVendorVersionDialog::add_version_in_list(wxWindow *parent,
                                                    const VendorAvailable &version,
                                                    wxGridBagSizer *versions_sizer,
                                                    const int line_num) {

    ////// version selector button //////
    if (m_vendor.is_installed && m_vendor.profile.config_version == version.config_version &&
        (m_vendor.profile.slicer_version == Semver::zero() || m_vendor.profile.slicer_version == version.slicer_version)) {
        // The installed version is a status, not an action. A panel gives it
        // the same visual footprint as the install buttons while keeping it
        // out of keyboard navigation and clearly marking the active version.
        wxPanel *installed_panel = new wxPanel(parent, wxID_ANY);
        wxBoxSizer *installed_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText *msg_version = new wxStaticText(installed_panel, wxID_ANY, version.config_version.to_string());
        installed_sizer->AddStretchSpacer();
        installed_sizer->Add(msg_version, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 5);
        installed_sizer->AddStretchSpacer();
        installed_panel->SetSizer(installed_sizer);
        installed_panel->SetToolTip(_L("This is the version currently installed."));
        msg_version->SetToolTip(_L("This is the version currently installed."));
        green_foreground_color.push_back(installed_panel);
        green_foreground_color.push_back(msg_version);
        versions_sizer->Add(installed_panel, wxGBPosition(line_num, 1), wxGBSpan(1, 1), wxEXPAND, 2);
    } else {
        wxButton *bt_version = new wxButton(parent, wxID_ANY, version.config_version.to_string());
        bt_version->Bind(wxEVT_BUTTON, ([this, version](wxCommandEvent &e) {
            this->begin_repository_operation(hscroll, _L("Installing the local preset. Please wait."));
            // install_vendor can work with copies passed as parameter, no worry.
            this->m_data.install_vendor(
                m_vendor.profile.id, version,
                repository_operation_callback<std::string>(
                    *this, [](ChooseVendorVersionDialog &dialog, std::string error_msg) {
                        dialog.finish_repository_operation(dialog.hscroll);
                        dialog.request_show_error_msg(error_msg);
                        dialog.request_rebuild_ui();
                    }));
        }));
        bt_version->SetToolTip(_L("Click this button to install this version of the vendor bundle."));
        versions_sizer->Add(bt_version, wxGBPosition(line_num, 1), wxGBSpan(1, 1), wxEXPAND, 2);
    }

    ////// slicer version //////
    wxStaticText *msg_slicer_version = new wxStaticText(parent, wxID_ANY, version.slicer_version.to_string());
    msg_slicer_version->SetToolTip(format(_L("This is the slicer version for which this bundle was built. Current version: %1%"), SLIC3R_VERSION_FULL));
    versions_sizer->Add(msg_slicer_version, wxGBPosition(line_num, 2), wxGBSpan(1, 1), wxEXPAND, 2);
    const Semver current_slicer_version = *Semver::parse(SLIC3R_VERSION_FULL);
    if (version.slicer_version > current_slicer_version) {
        // red
        red_foreground_color.push_back(msg_slicer_version);
    } else if (version.slicer_version == current_slicer_version) {
        // green
        green_foreground_color.push_back(msg_slicer_version);
    } else {
        // yellow
    }

    ////// log //////
    wxStaticText *msg_changelog = new wxStaticText(parent, wxID_ANY, version.notes);
    msg_changelog->SetToolTip(_L("This message includes the GitHub commit logs between the previous version and the current one."));
    versions_sizer->Add(msg_changelog, wxGBPosition(line_num, 3), wxGBSpan(1, 1), wxEXPAND, 2);

}

}} // namespace Slic3r::GUI
