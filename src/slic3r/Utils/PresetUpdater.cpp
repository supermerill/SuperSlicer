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

#include <boost/log/trivial.hpp>

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

PresetUpdater::~PresetUpdater()
{
    // Snapshot callbacks capture this GUI adapter. Drain the worker while all
    // of its AppConfig and PresetBundle references are still alive.
    m_snapshot_executor.shutdown_and_wait();
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
        post_to_gui([callback_result, update_count] { callback_result(update_count); });
    }, force);
}

void PresetUpdater::download_changelogs(const std::string &vendor_id,
                                        std::function<void(bool)> callback_result,
                                        bool force)
{
    m_core.download_changelogs(vendor_id, [this, callback_result = std::move(callback_result)](bool succeeded) {
        post_to_gui([callback_result, succeeded] { callback_result(succeeded); });
    }, force);
}

void PresetUpdater::download_new_repo(const std::string &rest_url,
                                      std::function<void(Slic3r::UpdaterError)> callback_result)
{
    m_core.download_new_repo(rest_url, [this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
        post_to_gui([callback_result, error = std::move(error)]() mutable {
            callback_result(std::move(error));
        });
    });
}

void PresetUpdater::cache_vendor_archive(const boost::filesystem::path &archive_path,
                                         std::function<void(const std::string &)> callback_result)
{
    m_core.cache_vendor_archive(
        archive_path,
        [this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
            dispatch_error_callback(callback_result, std::move(error));
        });
}

void PresetUpdater::cache_vendor_ini(const boost::filesystem::path &profile_path,
                                     std::function<void(const std::string &)> callback_result)
{
    m_core.cache_vendor_ini(
        profile_path,
        [this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
            dispatch_error_callback(callback_result, std::move(error));
        });
}

void PresetUpdater::uninstall_vendor(const std::string &vendor_id,
                                     std::function<void(Slic3r::UpdaterError)> callback_result)
{
    m_core.uninstall_vendor(vendor_id, [this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
        post_to_gui([callback_result, error = std::move(error)]() mutable {
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
        post_to_gui([callback_result, error = std::move(error)]() mutable {
            callback_result(std::move(error));
        });
    });
}

void PresetUpdater::uninstall_all_vendors(std::function<void(Slic3r::UpdaterError)> callback_result)
{
    m_core.uninstall_all_vendors([this, callback_result = std::move(callback_result)](Slic3r::UpdaterError error) {
        post_to_gui([callback_result, error = std::move(error)]() mutable {
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
    post_to_gui([this] { show_synch_window_internal(); });
}

void PresetUpdater::prepare_vendor_change_async(
    Slic3r::VendorChange change,
    const std::vector<std::string> &vendor_ids,
    Slic3r::PresetUpdaterHost::PrepareCallback callback)
{
    if (vendor_ids.empty()) {
        callback(Slic3r::UpdaterError(), std::string());
        return;
    }

    // AppConfig is owned by wx. Capture its small value state there before the
    // worker parses vendor files and recursively copies the snapshot payload.
    post_to_gui([this, change, callback = std::move(callback)]() mutable {
        const Config::Snapshot::Reason reason = change == Slic3r::VendorChange::Uninstall ?
            Config::Snapshot::SNAPSHOT_DOWNGRADE : Config::Snapshot::SNAPSHOT_UPGRADE;
        const std::string comment = change == Slic3r::VendorChange::Uninstall ?
            _u8L("Before removing vendor bundles") : _u8L("Before changing vendor bundles");

        const std::shared_ptr<std::optional<Config::Snapshot>> materialized =
            std::make_shared<std::optional<Config::Snapshot>>();
        try {
            Config::Snapshot captured = Config::SnapshotDB::singleton().capture_snapshot_state(
                *m_app.app_config, reason, comment);
            const bool accepted = m_snapshot_executor.enqueue(
                [materialized, captured = std::move(captured)]() mutable {
                    materialized->emplace(
                        Config::SnapshotDB::singleton().materialize_snapshot(std::move(captured)));
                    return Slic3r::UpdaterError();
                },
                [this, materialized, callback](Slic3r::UpdaterError error) mutable {
                    try {
                        post_to_gui([materialized, callback, error = std::move(error)]() mutable {
                            if (!error.succeeded()) {
                                callback(std::move(error), std::string());
                                return;
                            }
                            try {
                                const Config::Snapshot &snapshot =
                                    Config::SnapshotDB::singleton().register_snapshot(
                                        std::move(materialized->value()));
                                callback(Slic3r::UpdaterError(), snapshot.id);
                            } catch (...) {
                                callback(Slic3r::make_updater_error_from_exception(
                                             std::current_exception(),
                                             Slic3r::UpdaterError::Code::Filesystem),
                                         std::string());
                            }
                        });
                    } catch (...) {
                        callback(Slic3r::make_updater_error_from_exception(std::current_exception()),
                                 std::string());
                    }
                });
            if (!accepted)
                callback(Slic3r::make_updater_error(
                             Slic3r::UpdaterError::Code::PreparationRejected,
                             "The snapshot worker is shutting down."),
                         std::string());
        } catch (...) {
            callback(Slic3r::make_updater_error_from_exception(
                         std::current_exception(), Slic3r::UpdaterError::Code::Filesystem),
                     std::string());
        }
    });
}

void PresetUpdater::rollback_vendor_change_async(
    const std::string &token,
    Slic3r::PresetUpdaterHost::RollbackCallback callback)
{
    // SnapshotDB lookup and AppConfig publication remain on wx. The recursive
    // restoration between them runs on the dedicated snapshot worker.
    post_to_gui([this, token, callback = std::move(callback)]() mutable {
        const std::optional<Config::Snapshot> snapshot =
            Config::SnapshotDB::singleton().snapshot_copy(token);
        if (!snapshot) {
            callback(Slic3r::make_updater_error(
                Slic3r::UpdaterError::Code::Filesystem,
                "The vendor rollback snapshot was not found."));
            return;
        }

        const std::shared_ptr<Config::Snapshot> detached =
            std::make_shared<Config::Snapshot>(*snapshot);
        const bool accepted = m_snapshot_executor.enqueue(
            [detached] {
                Config::SnapshotDB::singleton().restore_snapshot_files(*detached);
                return Slic3r::UpdaterError();
            },
            [this, detached, callback](Slic3r::UpdaterError error) mutable {
                try {
                    post_to_gui([this, detached, callback, error = std::move(error)]() mutable {
                        if (!error.succeeded()) {
                            callback(std::move(error));
                            return;
                        }
                        try {
                            Config::SnapshotDB::singleton().apply_snapshot_configuration(
                                *detached, *m_app.app_config);
                            m_app.app_config->set("on_snapshot", detached->id);
                            m_app.preset_bundle->load_presets(
                                *m_app.app_config,
                                ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
                            m_app.load_current_presets();
                            callback(Slic3r::UpdaterError());
                        } catch (...) {
                            callback(Slic3r::make_updater_error_from_exception(
                                std::current_exception(), Slic3r::UpdaterError::Code::Filesystem));
                        }
                    });
                } catch (...) {
                    callback(Slic3r::make_updater_error_from_exception(std::current_exception()));
                }
            });
        if (!accepted)
            callback(Slic3r::make_updater_error(
                Slic3r::UpdaterError::Code::PreparationRejected,
                "The snapshot worker is shutting down."));
    });
}

void PresetUpdater::vendor_files_changed(Slic3r::PresetUpdater &,
                                         Slic3r::VendorChange change,
                                         const std::vector<std::string> &vendor_ids)
{
    // Queue the reload before the operation callback so the dialog sees the
    // refreshed state when it rebuilds its controls.
    post_to_gui([this, change, vendor_ids] { reload_application_presets(change, vendor_ids); });
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

    // The core publishes its detached VendorSync result before notifying this
    // host. Avoid rescanning cache and vendor directories on the wx thread;
    // only repository synchronization remains to refresh remote metadata.
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
    post_to_gui([callback_result, error = std::move(error)] {
        const std::string message = error.succeeded() ? std::string() : format_updater_error(error);
        callback_result(message);
    });
}

void PresetUpdater::dispatch_errors_callback(const std::function<void(const std::string &)> &callback_result,
                                             Slic3r::UpdaterErrors errors)
{
    post_to_gui([callback_result, errors = std::move(errors)] {
        std::string message;
        for (const Slic3r::UpdaterError &error : errors) {
            if (!message.empty())
                message += '\n';
            message += format_updater_error(error);
        }
        callback_result(message);
    });
}

void PresetUpdater::post_to_gui(std::function<void()> operation) noexcept
{
    if (wxTheApp == nullptr)
        return;

    try {
        m_app.CallAfter([operation = std::move(operation)]() mutable {
            try {
                operation();
            } catch (const std::exception &error) {
                BOOST_LOG_TRIVIAL(error) << "Preset updater wx continuation failed: " << error.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Preset updater wx continuation failed with an unknown exception.";
            }
        });
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(error) << "Failed posting preset updater work to wx: " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Failed posting preset updater work to wx with an unknown exception.";
    }
}

} // namespace Slic3r::GUI
