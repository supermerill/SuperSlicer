///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See PresetUpdater.hpp. This file is the only place where vendor repository
// changes touch wxWidgets, snapshots and the currently loaded presets. The
// core updater can therefore be reused by the console and future server code.

#include "slic3r/Utils/PresetUpdater.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include <wx/app.h>
#include <wx/window.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "slic3r/Config/Snapshot.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/UpdateDialogs.hpp"
#include "slic3r/GUI/UpdaterErrorMessages.hpp"

namespace Slic3r::GUI {

PresetUpdater::PresetUpdater(GUI_App &app)
    : m_app(app)
    , m_core(this)
{
}

void PresetUpdater::set_installed_vendors(const Slic3r::PresetBundle *preset_bundle)
{
    m_core.set_installed_vendors(preset_bundle);
}

void PresetUpdater::reload_all_vendors()
{
    m_core.reload_all_vendors();
}

void PresetUpdater::sync_async(std::function<void(int)> callback_result, bool force)
{
    m_core.sync_async([this, callback_result = std::move(callback_result)](int update_count) {
        m_app.CallAfter([callback_result, update_count] { callback_result(update_count); });
    }, force);
}

void PresetUpdater::download_changelogs(const std::string &vendor_id,
                                        std::function<void(bool)> callback_result,
                                        bool force)
{
    m_core.download_changelogs(vendor_id, [this, callback_result = std::move(callback_result)](bool succeeded) {
        m_app.CallAfter([callback_result, succeeded] { callback_result(succeeded); });
    }, force);
}

void PresetUpdater::download_new_repo(const std::string &rest_url,
                                      std::function<void(Slic3r::UpdaterError)> callback_result)
{
    m_core.download_new_repo(rest_url, [this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
        m_app.CallAfter([callback_result, error = std::move(error)]() mutable {
            callback_result(std::move(error));
        });
    });
}

void PresetUpdater::cache_vendor_archive(const boost::filesystem::path &archive_path,
                                         std::function<void(const std::string &)> callback_result)
{
    Slic3r::UpdaterError error = m_core.cache_vendor_archive(archive_path);
    if (error.succeeded())
        m_core.reload_all_vendors();
    dispatch_error_callback(callback_result, std::move(error));
}

void PresetUpdater::cache_vendor_ini(const boost::filesystem::path &profile_path,
                                     std::function<void(const std::string &)> callback_result)
{
    Slic3r::UpdaterError error = m_core.cache_vendor_ini(profile_path);
    if (error.succeeded())
        m_core.reload_all_vendors();
    dispatch_error_callback(callback_result, std::move(error));
}

void PresetUpdater::uninstall_vendor(const std::string &vendor_id,
                                     std::function<void(Slic3r::UpdaterError)> callback_result)
{
    m_core.uninstall_vendor(vendor_id, [this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
        m_app.CallAfter([callback_result, error = std::move(error)]() mutable {
            callback_result(std::move(error));
        });
    });
}

void PresetUpdater::install_vendor(const std::string &vendor_id,
                                   const Slic3r::VendorAvailable &version,
                                   std::function<void(const std::string &)> callback_result)
{
    m_core.install_vendor(vendor_id, version, [this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
        dispatch_error_callback(callback_result, std::move(error));
    });
}

void PresetUpdater::clear_cache_vendor(const std::string &vendor_id,
                                       std::function<void(Slic3r::UpdaterError)> callback_result)
{
    m_core.clear_cache_vendor(vendor_id, [this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
        m_app.CallAfter([callback_result, error = std::move(error)]() mutable {
            callback_result(std::move(error));
        });
    });
}

void PresetUpdater::uninstall_all_vendors(std::function<void(Slic3r::UpdaterError)> callback_result)
{
    m_core.uninstall_all_vendors([this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
        m_app.CallAfter([callback_result, error = std::move(error)]() mutable {
            callback_result(std::move(error));
        });
    });
}

void PresetUpdater::install_all_vendors(std::function<void(const std::string &)> callback_result)
{
    m_core.install_all_vendors([this, callback_result = std::move(callback_result)](Slic3r::UpdaterErrors errors) {
        dispatch_errors_callback(callback_result, std::move(errors));
    });
}

void PresetUpdater::upgrade_all_installed_vendors(std::function<void(const std::string &)> callback_result)
{
    m_core.upgrade_all_installed_vendors([this, callback_result = std::move(callback_result)](Slic3r::UpdaterErrors errors) {
        dispatch_errors_callback(callback_result, std::move(errors));
    });
}

int PresetUpdater::get_profile_count_to_update() const
{
    return m_core.get_profile_count_to_update();
}

size_t PresetUpdater::count_available() const
{
    return m_core.count_available();
}

size_t PresetUpdater::count_installed() const
{
    return m_core.count_installed();
}

bool PresetUpdater::is_synchronized() const
{
    return m_core.is_synchronized();
}

std::vector<Slic3r::VendorSync> PresetUpdater::vendors() const
{
    return m_core.vendors();
}

std::optional<Slic3r::VendorSync> PresetUpdater::vendor(const std::string &id) const
{
    return m_core.vendor(id);
}

void PresetUpdater::show_synch_window(wxWindow *parent,
                                      const wxString &message,
                                      std::function<void(bool)> callback_dialog_closed)
{
    {
        std::lock_guard<std::mutex> guard(m_dialog_mutex);
        m_dialog_parent = parent;
        m_dialog_message = message.utf8_string();
        m_dialog_callback = std::move(callback_dialog_closed);
    }

    if (m_core.count_available() == 0)
        m_core.reload_all_vendors();
    if (!m_core.is_synchronized()) {
        sync_async([this](int) { show_synch_window_internal(); });
        return;
    }
    m_app.CallAfter([this] { show_synch_window_internal(); });
}

std::optional<std::string> PresetUpdater::prepare_vendor_change(
    Slic3r::VendorChange change, const std::vector<std::string> &vendor_ids)
{
    if (vendor_ids.empty())
        return std::string();

    const Config::Snapshot::Reason reason = change == Slic3r::VendorChange::Uninstall ||
                                                     change == Slic3r::VendorChange::ClearCache ?
        Config::Snapshot::SNAPSHOT_DOWNGRADE : Config::Snapshot::SNAPSHOT_UPGRADE;
    const std::string comment = change == Slic3r::VendorChange::Uninstall ?
        _u8L("Before removing vendor bundles") : _u8L("Before changing vendor bundles");
    const Config::Snapshot *snapshot = Config::take_config_snapshot_report_error(
        *m_app.app_config, reason, comment);
    return snapshot == nullptr ? std::nullopt : std::optional<std::string>(snapshot->id);
}

Slic3r::UpdaterError PresetUpdater::rollback_vendor_change(const std::string &token)
{
    try {
        // Snapshot restoration puts both the complete vendor directory and the
        // user's preset selections back into their pre-operation state.
        const Config::Snapshot &snapshot = Config::SnapshotDB::singleton().restore_snapshot(
            token, *m_app.app_config);
        m_app.app_config->set("on_snapshot", snapshot.id);
        m_app.preset_bundle->load_presets(
            *m_app.app_config, ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        m_app.load_current_presets();
        return Slic3r::UpdaterError();
    } catch (const std::exception &error) {
        return Slic3r::make_updater_error(Slic3r::UpdaterError::Code::Filesystem, error.what());
    }
}

void PresetUpdater::dispatch_vendor_change(std::function<void()> operation)
{
    // Snapshot creation and preset publication touch GUI-owned application
    // state. Queue the complete transaction instead of running it in the HTTP
    // completion thread that prepared the package cache.
    m_app.CallAfter([operation = std::move(operation)]() mutable { operation(); });
}

void PresetUpdater::vendor_files_changed(Slic3r::PresetUpdater &,
                                         Slic3r::VendorChange change,
                                         const std::vector<std::string> &vendor_ids)
{
    // Queue the reload before the operation callback so the dialog sees the
    // refreshed state when it rebuilds its controls.
    m_app.CallAfter([this, change, vendor_ids] { reload_application_presets(change, vendor_ids); });
}

void PresetUpdater::reload_application_presets(Slic3r::VendorChange change, const std::vector<std::string> &vendor_ids)
{
    AppConfig::VendorMap configured_vendors = m_app.app_config->vendors();
    for (const std::string &vendor_id : vendor_ids) {
        if (change == Slic3r::VendorChange::Uninstall) {
            configured_vendors.erase(vendor_id);
            continue;
        }

        const std::optional<Slic3r::VendorSync> vendor = m_core.vendor(vendor_id);
        AppConfig::VendorMap::iterator configured_vendor = configured_vendors.find(vendor_id);
        if (!vendor.has_value() || configured_vendor == configured_vendors.end())
            continue;

        // Updating a bundle can remove models or variants. Drop only stale
        // selections so the rest of the user's configured printers survives.
        for (AppConfig::VendorMap::mapped_type::iterator model = configured_vendor->second.begin();
             model != configured_vendor->second.end();) {
            const VendorProfile::PrinterModel *profile_model = nullptr;
            for (const VendorProfile::PrinterModel &candidate : vendor->profile.models) {
                if (candidate.id == model->first) {
                    profile_model = &candidate;
                    break;
                }
            }
            if (profile_model == nullptr) {
                model = configured_vendor->second.erase(model);
                continue;
            }
            for (std::set<std::string>::iterator variant = model->second.begin(); variant != model->second.end();) {
                const bool exists = std::any_of(profile_model->variants.begin(), profile_model->variants.end(),
                    [&variant](const VendorProfile::PrinterVariant &candidate) { return candidate.name == *variant; });
                if (exists)
                    ++variant;
                else
                    variant = model->second.erase(variant);
            }
            if (model->second.empty())
                model = configured_vendor->second.erase(model);
            else
                ++model;
        }
    }

    m_app.app_config->set_vendors(std::move(configured_vendors));
    m_app.app_config->save();
    m_app.preset_bundle->load_installed_printers(*m_app.app_config);
    m_app.preset_bundle->load_presets(*m_app.app_config, ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
    m_app.load_current_presets();
    m_core.reload_all_vendors();
    m_core.sync_async([](int) {}, false);
}

void PresetUpdater::show_synch_window_internal()
{
    wxWindow *parent = nullptr;
    std::string message;
    std::function<void(bool)> callback_dialog_closed;
    {
        std::lock_guard<std::mutex> guard(m_dialog_mutex);
        parent = m_dialog_parent;
        message = std::move(m_dialog_message);
        callback_dialog_closed = std::move(m_dialog_callback);
        m_dialog_parent = nullptr;
    }
    if (parent == nullptr)
        return;

    UpdateConfigDialog dialog(parent, *this, Slic3r::GUI::from_u8(message));
    const int result = dialog.ShowModal();
    if (callback_dialog_closed)
        callback_dialog_closed(result == wxID_OK);
}

void PresetUpdater::dispatch_error_callback(const std::function<void(const std::string &)> &callback_result,
                                            Slic3r::UpdaterError error)
{
    m_app.CallAfter([callback_result, error = std::move(error)] {
        const std::string message = error.succeeded() ? std::string() : format_updater_error(error);
        callback_result(message);
    });
}

void PresetUpdater::dispatch_errors_callback(const std::function<void(const std::string &)> &callback_result,
                                             Slic3r::UpdaterErrors errors)
{
    m_app.CallAfter([callback_result, errors = std::move(errors)] {
        std::string message;
        for (const Slic3r::UpdaterError &error : errors) {
            if (!message.empty())
                message += '\n';
            message += format_updater_error(error);
        }
        callback_result(message);
    });
}

} // namespace Slic3r::GUI
