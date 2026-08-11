///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Superslicer 2025 Durand remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PresetUpdater.hpp"

#include "libslic3r/Plugins/PluginRepository.hpp"

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include <curl/curl.h>
#include <curl/curl.h>

#include <wx/app.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>

#include "libslic3r/format.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/Config/Snapshot.hpp"
#include "slic3r/Config/Version.hpp"
#include "slic3r/GUI/ConfigWizard.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/UpdateDialogs.hpp"
#include "libslic3r/Updater/Http.hpp"
namespace fs = boost::filesystem;
using Slic3r::GUI::Config::Index;
using Slic3r::GUI::Config::Version;
using Slic3r::GUI::Config::Snapshot;
using Slic3r::GUI::Config::SnapshotDB;

namespace Slic3r {

wxDEFINE_EVENT(EVT_CONFIG_UPDATER_SYNC_DONE, wxCommandEvent);
wxDEFINE_EVENT(EVT_CONFIG_UPDATER_SHOW_DIALOG, wxCommandEvent);

#define ERROR_MSG_UNABLE_SNAPSHOT "Error: fail to take a snapshot"
#define ERROR_MSG_UNABLE_COPY_CONFIG "Unable to copy the vendor bundle into the configuration directory."


bool copy_file_and_icons(boost::filesystem::path dir_in, boost::filesystem::path dir_out, std::string vendor_id, bool copy = true);

PresetUpdater::PresetUpdater(wxEvtHandler* event_handler) : evt_handler(event_handler){

    evt_handler->Bind(EVT_CONFIG_UPDATER_SHOW_DIALOG, [this](const wxCommandEvent& evt) {
        this->_show_synch_window_internal();
    });

    args_for_dialog.parent = nullptr;

}

PresetUpdater::~PresetUpdater() {
    if (thread.joinable()) {
        // This will stop transfers being done by the thread, if any.
        // Cancelling takes some time, but should complete soon enough.
        cancel = true;
        thread.join();
    }
}

void PresetUpdater::set_installed_vendors(const PresetBundle *preset_bundle) {
    std::lock_guard<std::recursive_mutex> guard(this->all_vendors_mutex);
    for (const auto& [id, installed_vendor] : preset_bundle->vendors) {
        auto it = all_vendors.find(installed_vendor.id);
        if (it == all_vendors.end()) {
            this->all_vendors[installed_vendor.id] = (VendorSync{installed_vendor, true});
            if (!installed_vendor.config_update_rest.empty()) {
                is_synch = false;
            }
        } else {
            bool has_cache = boost::filesystem::exists(GUI::into_path(data_dir()) / "cache" / "vendor" / installed_vendor.usable_id());
            it->second.reset(installed_vendor, true, has_cache);
        }
    }
}

void PresetUpdater::load_unused_vendors(std::set<std::string> &vendors_id, const boost::filesystem::path vendor_dir, bool is_installed) {
    std::vector<boost::filesystem::path> vendors;
    if (boost::filesystem::exists(vendor_dir)) {
        for (const boost::filesystem::directory_entry &prg_dir : boost::filesystem::directory_iterator(vendor_dir)) {
            if (prg_dir.status().type() == boost::filesystem::file_type::regular_file) {
                boost::filesystem::path file_path = prg_dir.path();
                //assert(file_path.extension() == ".ini");
                if (file_path.extension() != ".ini") {
                    continue;
                }
                //if (vendors_id.find(file_path.stem().string()) == vendors_id.end()) {
                    vendors.push_back(file_path);
                //}
            }
        }
    }
    // read them
    std::lock_guard<std::recursive_mutex> guard(this->all_vendors_mutex);
    for (boost::filesystem::path &path : vendors) {
        VendorProfile vp = VendorProfile::from_ini(path, false);
        auto it = all_vendors.find(vp.id);
        if (it == all_vendors.end() || it->second.available_profiles.empty()) {
            if (!vp.config_update_rest.empty()) {
                is_synch = false;
            }
            // ensure it's copied into the cache
            bool has_cache = boost::filesystem::exists(GUI::into_path(data_dir()) / "cache" / "vendor" / vp.usable_id());
            this->all_vendors[vp.id] = VendorSync{vp, is_installed, has_cache};
            assert(this->all_vendors[vp.id].is_installed == is_installed);
            if (vp.config_version != Semver()) {
                // this vendor spec has a version
                this->all_vendors[vp.id].available_profiles.emplace_back(
                    VendorAvailable{vp.config_version, vp.slicer_version, path.string(), "", "", "",
                                    /*tag = */vp.config_version.to_string() + "=" + vp.slicer_version.to_string(), ""});
            }
            vendors_id.insert(vp.id);
            // now order by version
            this->all_vendors[vp.id].sort_available();
        } else {
            // add new available version if not present
            bool new_version = true;
            for (const VendorAvailable &version : it->second.available_profiles) {
                if (version.config_version == vp.config_version) {
                    new_version = false;
                    break;
                }
            }
            if (new_version) {
                it->second.available_profiles.emplace_back(
                    VendorAvailable{vp.config_version, vp.slicer_version, path.string(), "", "", "",
                                    /*tag = */vp.config_version.to_string() + "=" + vp.slicer_version.to_string(), ""});
                it->second.sort_available();
                it->second.can_upgrade = !it->second.available_profiles.empty() && it->second.available_profiles.front().config_version > it->second.profile.config_version;
            }
        }
    }
}

void PresetUpdater::reload_all_vendors() {
    //this->unused_vendors.clear();
    std::set<std::string> vendors_id;

    boost::filesystem::path configuration_path(Slic3r::data_dir());
    boost::filesystem::path resources_path(Slic3r::resources_dir());
    all_vendors.clear();

    // read all vendor from configuration
    load_unused_vendors(vendors_id, configuration_path / "vendor", /*is_installed=*/ true);

    {
        std::lock_guard<std::recursive_mutex> guard(this->all_vendors_mutex);
        for (const auto &[id, vendor_synch] : this->all_vendors) {
            if (vendor_synch.is_installed) {
                vendors_id.insert(vendor_synch.profile.id);
            }
        }
    }
    // if the ini is in the root instead of its dir, fix the issue.
    for (const boost::filesystem::directory_entry &vendor_entry :
         boost::filesystem::directory_iterator(configuration_path / "cache" / "vendor")) {
        if (vendor_entry.status().type() == boost::filesystem::file_type::regular_file) {
            if (vendor_entry.path().extension() == ".ini") {
                boost::filesystem::path vendor_dir = vendor_entry.path().parent_path();
                if (boost::filesystem::exists(vendor_dir / vendor_entry.path().stem()) &&
                    boost::filesystem::is_directory(vendor_dir / vendor_entry.path().stem())) {
                    // rename for simplier copy
                    boost::filesystem::rename(vendor_dir / vendor_entry.path().stem(),
                                              vendor_dir / (vendor_entry.path().stem().string() + ".temp"));
                    boost::filesystem::create_directory(vendor_dir / vendor_entry.path().stem());
                    // move
                    boost::filesystem::rename(vendor_dir / (vendor_entry.path().stem().string() + ".temp"),
                                              vendor_dir / vendor_entry.path().stem() / vendor_entry.path().stem());
                } else {
                    boost::filesystem::create_directory(vendor_dir / vendor_entry.path().stem());
                }
                boost::filesystem::rename(vendor_entry.path(),
                                            vendor_dir / vendor_entry.path().stem() / vendor_entry.path().filename());
            }
        }
    }
    // read all vendor from cache (added but not installed)
    for (const boost::filesystem::directory_entry &vendor_entry :
         boost::filesystem::directory_iterator(configuration_path / "cache" / "vendor")) {
        if (vendor_entry.status().type() == boost::filesystem::file_type::directory_file) {
            load_unused_vendors(vendors_id, vendor_entry.path(), /*is_installed=*/false);
        }
    }

    // get our vendors from resources;
    assert(boost::filesystem::exists(resources_path));
    if (!boost::filesystem::exists(resources_path / "profiles")) {
        boost::filesystem::create_directories(resources_path / "profiles");
    }
    //load_unused_vendors(vendors_id, resources_path / "profiles", /*is_installed=*/ false);
    for (const boost::filesystem::directory_entry &vendor_entry :
         boost::filesystem::directory_iterator(resources_path / "profiles")) {
        if (vendor_entry.path().extension() != ".ini") {
            continue;
        }
        try {
            VendorProfile vp = VendorProfile::from_ini(vendor_entry.path(), false);
            //if (vp.config_update_rest.empty()) 
            {
                // copy to cache if not already in
                boost::filesystem::path vendor_cache = repository_package_cache_path(
                    GUI::into_path(data_dir()), RepositoryPackageType::Vendor, vp.usable_id(),
                    vp.config_version.to_string(), vp.slicer_version.to_string());
                std::string vendor_file_id = vendor_entry.path().stem().string();
                if (!boost::filesystem::exists(vendor_cache)) {
                    boost::filesystem::create_directories(vendor_cache / "profiles");
                    copy_file_and_icons(resources_path / "profiles", vendor_cache / "profiles", vendor_file_id, true);
                }
            }
        } catch (std::exception) {
        }
    }

    // load all available profile copied into cache.
    for (const boost::filesystem::directory_entry &vendor_entry :
         boost::filesystem::directory_iterator(configuration_path / "cache" / "vendor")) {
        if (vendor_entry.is_directory()) {
            if (boost::filesystem::exists(vendor_entry.path() / "profiles")) {
                load_unused_vendors(vendors_id, vendor_entry.path() / "profiles", /*is_installed=*/false);
                continue;
            }
            for (const boost::filesystem::directory_entry &config_entry :
                 boost::filesystem::directory_iterator(vendor_entry.path())) {
                if (config_entry.is_directory() && boost::filesystem::exists(config_entry.path() / "profiles")) {
                    load_unused_vendors(vendors_id, config_entry.path() / "profiles", /*is_installed=*/false);
                }
            }
        }
    }



}

void PresetUpdater::download_logs(const std::string &vendor_id, std::function<void(bool)> callback_result, bool force) {
    bool result = true;
    try {
        std::lock_guard<std::recursive_mutex> guard_vendors(this->all_vendors_mutex);
        auto it_mutable_vendor = all_vendors.find(vendor_id);
        if (it_mutable_vendor == all_vendors.end()) {
            assert(false);
            result = false;
            goto callback_after_unlock;
            //note: you can use guard.unlock();[guard_vendors.unlock()]callback_result(result);return; instead of the goto.
        }
        assert(vendor_id == it_mutable_vendor->second.profile.id);
        VendorSync &vendor = it_mutable_vendor->second;
        // note: all_vendors shoudln't change since vendor was stored somewhere elese.
        std::lock_guard<std::mutex> guard(this->callback_update_changelog_mutex);
        changelog_synch = vendor.available_profiles.size();
        boost::filesystem::path cache_path = GUI::into_path(data_dir()) / "cache" / "vendor" / vendor.profile.usable_id() / "logs";
        boost::filesystem::create_directories(cache_path);
        // for each tag
        for (size_t available_idx = 0; available_idx < vendor.available_profiles.size(); available_idx++) {
            VendorAvailable &version = vendor.available_profiles[available_idx];
            if (version.commit_url.empty()) {
                if (--changelog_synch == 0) {
                    assert(available_idx + 1 == vendor.available_profiles.size());
                    goto callback_after_unlock;
                }
                continue;
            }
            Semver slicer_major_ver = version.slicer_version.no_patch();
            // is there older versions with same slicer major version?
            Semver best_previous_version = Semver::zero();
            size_t best_idx = 0;
            for (size_t i_prev = 0; i_prev < vendor.available_profiles.size(); i_prev++) {
                if (!vendor.available_profiles[i_prev].commit_sha.empty() &&
                    vendor.available_profiles[i_prev].config_version < version.config_version) {
                    if (best_previous_version < vendor.available_profiles[i_prev].config_version) {
                        if (vendor.available_profiles[i_prev].slicer_version.no_patch() == slicer_major_ver) {
                            best_idx = i_prev;
                            best_previous_version = vendor.available_profiles[i_prev].config_version;
                        }
                    }
                }
            }
            if (best_idx == 0) {
                assert(best_previous_version == Semver::zero());
                // can't find a previous version with same slicer version, choose best one
                // find the previous available profile (if any)
                for (size_t i_prev = 0; i_prev < vendor.available_profiles.size(); i_prev++) {
                    if (!vendor.available_profiles[i_prev].commit_sha.empty() &&
                        vendor.available_profiles[i_prev].config_version < version.config_version) {
                        if (best_previous_version < vendor.available_profiles[i_prev].config_version) {
                            if (vendor.available_profiles[i_prev].slicer_version.no_patch() <= slicer_major_ver) {
                                best_idx = i_prev;
                                best_previous_version = vendor.available_profiles[i_prev].config_version;
                            }
                        }
                    }
                }
            }
            if (best_idx == 0) {
                boost::filesystem::path file_cache_path = cache_path / (version.tag + ".json");
                if (boost::filesystem::exists(file_cache_path) && !force) {
                    // reuse the cache file
                    boost::nowide::ifstream file(file_cache_path.string());
                    std::string body{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
                    boost::property_tree::ptree root;
                    std::stringstream json_stream(body);
                    boost::property_tree::read_json(json_stream, root);
                    version.notes = root.get<std::string>("commit.message");
                    if (--changelog_synch == 0) {
                        goto callback_after_unlock;
                    }
                } else if (has_api_request_slot(version.commit_url)) {
                    // root
                    Http::get(version.commit_url)
                        .size_limit(1024 * 1024 * 4 /*a commit json can be 512kio*/)
                        .on_error([this, &version, callback_result](std::string body, std::string error,
                                                                    unsigned http_status) {
                            BOOST_LOG_TRIVIAL(error) << "PresetUpdater::download_logs: Couldn't download "
                                                        << version.commit_url << ": "<< error;
                            if (--changelog_synch == 0) {
                                callback_result(false);
                            }
                        })
                        .on_complete([this, &version, file_cache_path, callback_result](std::string body,
                                                                                        unsigned /* http_status */) {
                            // store
                            try {
                                std::ofstream file_out;
                                file_out.open(file_cache_path.string().c_str(),
                                              std::ofstream::out | std::ofstream::trunc);
                                file_out << body;
                                file_out.close();
                            } catch (const std::exception &err) {
                                BOOST_LOG_TRIVIAL(error)
                                    << "PresetUpdater::download_logs: Couldn't open "
                                    << file_cache_path.string().c_str() << " for writing: " << err.what();
                            }
                            // parse it
                            try {
                                boost::property_tree::ptree root;
                                std::stringstream json_stream(body);
                                boost::property_tree::read_json(json_stream, root);
                                version.notes = root.get<std::string>("commit.message");
                                if (--changelog_synch == 0) {
                                    callback_result(true);
                                }
                            } catch (const std::exception &err) {
                                BOOST_LOG_TRIVIAL(error)
                                    << "PresetUpdater::download_logs: Parse Error: " << err.what();
                            }
                        })
                        .perform();
                }
            } else {
                boost::filesystem::path file_cache_path = cache_path /
                    (vendor.available_profiles[best_idx].tag + "..." + version.tag + ".json");
                if (boost::filesystem::exists(file_cache_path) && !force) {
                    // reuse the cache file
                    boost::nowide::ifstream file(file_cache_path.string());
                    std::string body{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
                    boost::property_tree::ptree root;
                    std::stringstream json_stream(body);
                    boost::property_tree::read_json(json_stream, root);
                    version.notes.clear();
                    for (boost::property_tree::ptree::value_type &kv : root.get_child("commits")) {
                        if (version.notes.empty()) {
                            version.notes += kv.second.get<std::string>("commit.message");
                        } else {
                            version.notes = kv.second.get<std::string>("commit.message") + "\n" + version.notes;
                        }
                    }
                    if (--changelog_synch == 0) {
                        goto callback_after_unlock;
                    }
                } else if (std::string base_url = VendorProfile::get_http_url_rest(vendor.profile.config_update_rest);
                           has_api_request_slot(base_url)) {
                    std::string url(base_url + "/compare/" + vendor.available_profiles[best_idx].tag + "..." +
                                    version.tag);
                    Http::get(url)
                        .size_limit(1024 * 128 /*128kio, 100tags should use 27ko*/)
                        .on_error([this, &version, callback_result](std::string body, std::string error,
                                                                    unsigned http_status) {
                            if (--changelog_synch == 0) {
                                callback_result(false);
                            }
                        })
                        .on_complete([this, &version, file_cache_path, callback_result](std::string body,
                                                                                        unsigned /* http_status */) {
                            // store
                            try {
                                std::ofstream file_out;
                                file_out.open(file_cache_path.string().c_str(),
                                              std::ofstream::out | std::ofstream::trunc);
                                file_out << body;
                                file_out.close();
                            } catch (const std::exception &err) {
                                BOOST_LOG_TRIVIAL(error)
                                    << "PresetUpdater::download_logs: Couldn't open "
                                    << file_cache_path.string().c_str() << " for writing: " << err.what();
                            }
                            // parse it
                            try {
                                boost::property_tree::ptree root;
                                std::stringstream json_stream(body);
                                boost::property_tree::read_json(json_stream, root);
                                version.notes.clear();
                                for (boost::property_tree::ptree::value_type &kv : root.get_child("commits")) {
                                    if (version.notes.empty()) {
                                        version.notes += kv.second.get<std::string>("commit.message");
                                    } else {
                                        version.notes = kv.second.get<std::string>("commit.message") + "\n" +
                                            version.notes;
                                    }
                                }
                            } catch (const std::exception &err) {
                                BOOST_LOG_TRIVIAL(error) << "PresetUpdater::download_logs: Parse Error: " << err.what();
                            }
                            if (--changelog_synch == 0) {
                                callback_result(true);
                            }
                        })
                        .perform();
                }
            }
        }
    } catch (const std::exception &err) {
        BOOST_LOG_TRIVIAL(error) << "PresetUpdater::download_logs: Parse Error: " << err.what();
    }
    return;
callback_after_unlock:
    callback_result(result);
}

void PresetUpdater::sync_async(std::function<void(int)> callback_for_after_update_preset, bool force) {
    if (this->all_vendors.empty()) {
        callback_for_after_update_preset(get_profile_count_to_update());
        // already done
        return;
    }
    bool error = false;
    {
        std::lock_guard<std::mutex> guard(this->callback_update_preset_mutex);
        bool already_in_synch = synch_process_ongoing.exchange(true);
        if (already_in_synch) {
            assert(false);
            error = true;
            this->callback_update_preset = [](int) {};
        } else {
            this->callback_update_preset = callback_for_after_update_preset;
        }
    }
    if (error) {
        callback_for_after_update_preset(get_profile_count_to_update());
        return;
    }
    std::lock_guard<std::recursive_mutex> vendors_guard(this->all_vendors_mutex);
    profiles_synch = this->all_vendors.size();
    for (auto& [id, vendor_synch] : this->all_vendors) {
        this->update_vendor(vendor_synch, force);
    }
}

void PresetUpdater::update_vendor(VendorSync &vendor, bool force) {
    // reuse
    boost::filesystem::path cache_path = GUI::into_path(data_dir()) / "cache" / "vendor" / vendor.profile.usable_id() / (vendor.profile.usable_id() + "_tags.json");
    if (boost::filesystem::exists(cache_path) && !force) {
        std::time_t last_mod  = boost::filesystem::last_write_time(cache_path);
        std::time_t now = std::time(nullptr);
        // wait a day before checking again
        if (last_mod + 24 * 3600 > now) {
            // reuse the cache file
            boost::nowide::ifstream file(cache_path.string());
            std::string body { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
            vendor.parse_tags(body);
            // end it
            vendor.synch_in_progress = false;
            end_updating();
            return;
        }
    }
    vendor.synch_in_progress = true;
    if (VendorProfile::get_http_url_rest(vendor.profile.config_update_rest).empty()) {
        //not correct config_update_github
        vendor.synch_failed = true;
        vendor.synch_in_progress = false;
        end_updating();
        return;
    }
    boost::filesystem::path cache_dir = GUI::into_path(data_dir()) / "cache" / "vendor" / vendor.profile.usable_id();
    boost::filesystem::create_directories(cache_dir);
    // create url to get tags from github
    std::string url(VendorProfile::get_http_url_rest(vendor.profile.config_update_rest) + "/tags?per_page=100;page=1");
    if (has_api_request_slot(url)) {
        Http::get(url)
            .size_limit(1024 * 64 /*64kio, 100tags should use 27ko*/)
            .on_error([&](std::string body, std::string error, unsigned http_status) {
                vendor.synch_failed = true;
                vendor.synch_in_progress = false;

                end_updating();
            })
            .on_complete([this, cache_path, &vendor](std::string body, unsigned /* http_status */) {
                // store it
                try {
                    std::ofstream file_out;
                    file_out.open(cache_path.string().c_str(), std::ofstream::out | std::ofstream::trunc);
                    file_out << body;
                    file_out.close();
                } catch (const std::exception &err) {
                    BOOST_LOG_TRIVIAL(error) << "PresetUpdater::install_new_repo: Couldn't open "
                                             << cache_path.string().c_str() << " for writing: " << err.what();
                }
                // parse it
                vendor.parse_tags(body);

                // end it
                vendor.synch_in_progress = false;
                end_updating();
            })
            .perform();
    } else {
        vendor.synch_failed = true;
        vendor.synch_in_progress = false;
        end_updating();
    }
}

void VendorSync::reset(const VendorProfile &vprofile, bool installed, bool has_cache) {
    this->is_installed = installed;
    this->has_cache = has_cache;
    this->synch_in_progress = false;
    this->synch_failed = false;
    //available_profiles = {};
    this->profile = vprofile;
    this->can_upgrade = !available_profiles.empty() && available_profiles.front().config_version > profile.config_version;
}

bool VendorSync::parse_tags(const std::string &json) {
    std::vector<RepositoryPackageVersion> repository_versions;
    std::string error_message;
    if (!parse_repository_versions(json, repository_versions, error_message)) {
        BOOST_LOG_TRIVIAL(warning) << error_message;
        return false;
    }
    std::map<std::string, size_t> versions_here;
    for (size_t idx = 0; idx < available_profiles.size(); ++idx) {
        VendorAvailable &version = available_profiles[idx];
        versions_here[version.config_version.to_string()+"="+version.slicer_version.to_string()] = idx;
    }

    for (const RepositoryPackageVersion &repository_version : repository_versions) {
        const std::optional<Semver> config_version = Semver::parse(repository_version.package_version);
        const std::optional<Semver> slicer_version = Semver::parse(repository_version.slicer_version);
        if (!config_version || !slicer_version)
            continue;
        if (versions_here.find(repository_version.tag) != versions_here.end()) {
            assert(versions_here[repository_version.tag] < available_profiles.size());
            //update the tag infos if not present
            VendorAvailable &to_update = available_profiles[versions_here[repository_version.tag]];
            //assert(!to_update.url_zip.empty());
            if (to_update.tag.empty() || to_update.url_zip.empty()) {
                to_update.tag = repository_version.tag;
                to_update.url_zip = repository_version.url_zip;
                to_update.commit_sha = repository_version.commit_sha;
                to_update.commit_url = repository_version.commit_url;
            }
        } else {
            // okay, add it
            versions_here[repository_version.tag] = available_profiles.size();
            available_profiles.emplace_back(VendorAvailable{*config_version, *slicer_version, std::string(),
                                                            repository_version.url_zip, repository_version.commit_sha,
                                                            repository_version.commit_url, repository_version.tag, std::string()});
        }
    }

    // now order by version
    sort_available();

    this->can_upgrade = !available_profiles.empty() && available_profiles.front().config_version > profile.config_version;
    this->is_synch = true;
    return this->can_upgrade;
}

void VendorSync::sort_available() {
    if (available_profiles.empty()) {
        return;
    }
    std::sort(available_profiles.begin(), available_profiles.end(),
              [](const VendorAvailable &e1, const VendorAvailable &e2) {
                  if (e1.slicer_version == e2.slicer_version) {
                      return e1.config_version > e2.config_version;
                  } else {
                      return e1.slicer_version > e2.slicer_version;
                  }
              });
    // choose best (ie first with compatible slicer)
    Semver current_slicer_version = *Semver::parse(SLIC3R_VERSION_FULL);
    Semver best_available_slicer_version = available_profiles.front().slicer_version;
    for (VendorAvailable &version : available_profiles) {
        if (version.slicer_version <= current_slicer_version &&
            version.slicer_version >= best_available_slicer_version) {
            best_available_slicer_version = version.slicer_version;
        }
    }
    for (VendorAvailable &version : available_profiles) {
        if (version.slicer_version == best_available_slicer_version) {
            if (best == nullptr || best->config_version < version.config_version) {
                best = &version;
            }
        }
    }
}

void PresetUpdater::end_updating() {
    int value = --profiles_synch;
    assert(value >= 0);
    if (value == 0) {
        // copy callback_update_preset to not call it inside a critical section
        std::function<void(int)> saved_callback_update_preset;
        {
            std::lock_guard<std::mutex> guard(this->callback_update_preset_mutex);
            saved_callback_update_preset = this->callback_update_preset;
            this->callback_update_preset = [](int) {};
        }
        // nobody increase it and the min is 0 so no race condition here.
        // end of synhc, emit callback
        saved_callback_update_preset(int(get_profile_count_to_update()));
        //wxCommandEvent *evt = new wxCommandEvent(EVT_CONFIG_UPDATER_SYNC_DONE);
        //evt->SetInt(profiles_to_update);
        //evt_handler->QueueEvent(evt);
        is_synch = true;
        synch_process_ongoing = false;
    }
}

int PresetUpdater::get_profile_count_to_update() {
    int count = 0;
    std::lock_guard<std::recursive_mutex> guard(this->all_vendors_mutex);
    for (const auto &[id, vendor_synch] : this->all_vendors) {
        if (vendor_synch.can_upgrade) {
            count++;
        }
    }
    return count;
}

int PresetUpdater::update_count()
{
    return get_profile_count_to_update();
}

void PresetUpdater::on_sync_completed()
{
    is_synch = true;
}

void PresetUpdater::uninstall_vendor(const std::string &vendor_id, std::function<void(bool)> callback_result) {
    // get vendor from map to remove "const"
    std::unique_lock<std::recursive_mutex> guard(this->all_vendors_mutex);
    auto it_mutable_vendor = all_vendors.find(vendor_id);
    if (it_mutable_vendor == all_vendors.end()) {
        assert(false);
        guard.unlock();
        callback_result(false);
        return;
    }
    assert(vendor_id == it_mutable_vendor->second.profile.id);
    const GUI::Config::Snapshot* snapshot = take_config_snapshot_report_error(*GUI::wxGetApp().app_config, GUI::Config::Snapshot::SNAPSHOT_DOWNGRADE,
        format(_u8L("Before removing vendor bundle '%1%'"), it_mutable_vendor->second.profile.full_name));
    if (!snapshot) {
        // fail to snapshot, cancel
        BOOST_LOG_TRIVIAL(error) << "Error: can't take snapshot, cancel preset removal.";
        guard.unlock();
        callback_result(false);
        return;
    }
    bool result = it_mutable_vendor->second.uninstall_vendor_config();
    if (result) {
        // unregister from appconfig
        auto vmap = GUI::wxGetApp().app_config->vendors();
        vmap.erase(vendor_id);
        GUI::wxGetApp().app_config->set_vendors(vmap);
        GUI::wxGetApp().app_config->save();
        // unregister from current slicer app
        GUI::wxGetApp().preset_bundle->load_installed_printers(*GUI::wxGetApp().app_config);
        GUI::wxGetApp().preset_bundle->load_presets(*GUI::wxGetApp().app_config, ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        // update tabs
        GUI::wxGetApp().load_current_presets();
        reload_all_vendors();
        sync_async([callback_result](int nbupdates) { callback_result(true); });
    } else {
        guard.unlock();
        callback_result(false);
    }
}

void PresetUpdater::clear_cache_vendor(const std::string &vendor_id, std::function<void(bool)> callback_result) {
    // get vendor from map to remove "const"
    std::unique_lock<std::recursive_mutex> guard(this->all_vendors_mutex);
    auto it_mutable_vendor = all_vendors.find(vendor_id);
    if (it_mutable_vendor == all_vendors.end()) {
        assert(false);
        guard.unlock();
        callback_result(false);
        return;
    }
    assert(vendor_id == it_mutable_vendor->second.profile.id);
    const GUI::Config::Snapshot* snapshot = take_config_snapshot_report_error(*GUI::wxGetApp().app_config, GUI::Config::Snapshot::SNAPSHOT_DOWNGRADE,
        format(_u8L("Before clearing vendor bundle '%1%' cache"), it_mutable_vendor->second.profile.full_name));
    if (!snapshot) {
        // fail to snapshot, cancel
        BOOST_LOG_TRIVIAL(error) << "Error: can't take snapshot, cancel preset removal.";
        guard.unlock();
        callback_result(false);
        return;
    }
    bool result = it_mutable_vendor->second.clear_cache();
    if (result) {
        guard.unlock();
        reload_all_vendors();
        callback_result(false);
    }
}

void PresetUpdater::install_vendor(const std::string &vendor_id,
                                   const VendorAvailable &version,
                                   std::function<void(const std::string &)> callback_result) {
    // get vendor from map to remove "const"
    std::unique_lock<std::recursive_mutex> guard(this->all_vendors_mutex);
    auto it_mutable_vendor = all_vendors.find(vendor_id);
    if (it_mutable_vendor == all_vendors.end()) {
        assert(false);
        guard.unlock();
        callback_result(_u8L("Unable to find the select vendor in the cache. Something went wrong with the software.")); 
        return;
    }
    assert(vendor_id == it_mutable_vendor->second.profile.id);
    const GUI::Config::Snapshot* snapshot = take_config_snapshot_report_error(*GUI::wxGetApp().app_config, GUI::Config::Snapshot::SNAPSHOT_UPGRADE,
        format(_u8L("Before installing vendor bundle '%1%' version %2%"), it_mutable_vendor->second.profile.full_name, version.config_version.to_string()));
    if (!snapshot) {
        // fail to snapshot, cancel
        BOOST_LOG_TRIVIAL(error) << "Error: can't take snapshot, cancel preset installation.";
        guard.unlock();
        callback_result(_u8L(ERROR_MSG_UNABLE_SNAPSHOT)); 
        return;
    }
    VendorSync &vendor = it_mutable_vendor->second;
    std::string result = vendor.install_vendor_config(version, *this);
    auto vmap = GUI::wxGetApp().app_config->vendors();
    if (result.empty() && vmap.find(vendor.profile.id) != vmap.end()) {
        // the vendor is already in use, update the slicer
        // read it with models
        if (vendor.profile.models.empty()) {
            assert(vendor.is_installed);
            vendor.profile = VendorProfile::from_ini(GUI::into_path(data_dir()) / "vendor" / (vendor.profile.id + ".ini"), true);
            assert(!vendor.profile.models.empty());
        }
        // update appconfig : remove unfindable printers
        for (auto printer_it = vmap[vendor.profile.id].begin(); printer_it != vmap[vendor.profile.id].end();) {
            // search for PrinterModel
            const VendorProfile::PrinterModel *printer_model = nullptr;
            for (const VendorProfile::PrinterModel &pm : vendor.profile.models) {
                if (pm.id == printer_it->first) {
                    printer_model = &pm;
                    break;
                }
            }
            if (printer_model == nullptr) {
                printer_it = vmap[vendor.profile.id].erase(printer_it);
            } else {
                // same for variant
                for (auto it_variant = printer_it->second.begin(); it_variant != printer_it->second.end();) {
                    bool found_name = false;
                    for (const VendorProfile::PrinterVariant &variant : printer_model->variants) {
                        if (variant.name == *it_variant) {
                            found_name = true;
                            break;
                        }
                    }
                    if (!found_name) {
                        it_variant = printer_it->second.erase(it_variant);
                    } else {
                        it_variant++;
                    }
                }
                // erase if no variant anymore
                if (printer_it->second.empty()) {
                    printer_it = vmap[vendor.profile.id].erase(printer_it);
                } else {
                    // next elt
                    ++printer_it;
                }
            }
        }
        GUI::wxGetApp().app_config->set_vendors(vmap);
        GUI::wxGetApp().app_config->save();
        // reload slicer app
        GUI::wxGetApp().preset_bundle->load_installed_printers(*GUI::wxGetApp().app_config);
        GUI::wxGetApp().preset_bundle->load_presets(*GUI::wxGetApp().app_config,
                                                    ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        // update tabs
        GUI::wxGetApp().load_current_presets();
    }
    guard.unlock();
    callback_result(result); 
}

bool copy_file_and_icons(boost::filesystem::path dir_in, boost::filesystem::path dir_out, std::string vendor_id, bool copy) {

    // copy the file & icons
    assert(boost::filesystem::exists(dir_in));
    if (boost::filesystem::exists(dir_in)) {
        boost::filesystem::path vendor_profile_file_in = dir_in / (vendor_id + ".ini");
        boost::filesystem::path vendor_profile_file_out = dir_out / (vendor_id + ".ini");
        assert(boost::filesystem::exists(vendor_profile_file_in));
        if (!boost::filesystem::exists(vendor_profile_file_in)) {
            return false;
        }
        if (copy) {
            // need to copy to a temp file and then rename to have a safe overwrite.
            boost::filesystem::copy(vendor_profile_file_in, dir_out / (vendor_id + ".new.ini"));
            boost::filesystem::rename(dir_out / (vendor_id + ".new.ini"), vendor_profile_file_out);
        } else {
            boost::filesystem::rename(vendor_profile_file_in, vendor_profile_file_out);
        }
        // now copy icons
        boost::filesystem::path icon_dir_in = dir_in / vendor_id;
        boost::filesystem::path icon_dir_out = dir_out / vendor_id;
        if (boost::filesystem::exists(icon_dir_in)) {
            // remove all
            if (boost::filesystem::exists(icon_dir_out)) {
                for (const boost::filesystem::directory_entry &path_entry :
                        boost::filesystem::directory_iterator(icon_dir_out)) {
                    boost::filesystem::remove_all(path_entry.path());
                }
            }
            boost::filesystem::create_directories(icon_dir_out);
            // copy all
            for (const boost::filesystem::directory_entry &path_entry :
                    boost::filesystem::directory_iterator(icon_dir_in)) {
                assert(path_entry.status().type() == boost::filesystem::file_type::regular_file);
                if (copy) {
                    boost::filesystem::path out_path = icon_dir_out / path_entry.path().lexically_relative(icon_dir_in);
                    boost::filesystem::path out_dir_path = out_path.parent_path();

                    boost::filesystem::copy(path_entry.path(), out_dir_path / "file.temp");
                    boost::filesystem::rename(out_dir_path / "file.temp", out_path);
                } else {
                    boost::filesystem::rename(path_entry.path(),
                                              icon_dir_out / path_entry.path().lexically_relative(icon_dir_in));
                }
            }
            if (!copy) {
                boost::filesystem::remove_all(icon_dir_in);
            }
        }
    }
    return true;

}

std::string VendorSync::install_vendor_config(const VendorAvailable &to_install, PresetUpdater& api_slot) {
    // rename current venddor file (if any)
    boost::filesystem::path vendor_file = GUI::into_path(data_dir()) / "vendor" / (this->profile.id + ".ini");
    assert(!this->is_installed || boost::filesystem::exists(vendor_file));
    // get new file
    if (!to_install.local_file.empty() && boost::filesystem::exists(to_install.local_file)) {
        // file in resource directory
        boost::filesystem::path new_vendor_file(to_install.local_file);
        assert(new_vendor_file.stem() == profile.id);
        bool copy_okay = copy_file_and_icons(new_vendor_file.parent_path(), GUI::into_path(data_dir()) / "vendor", new_vendor_file.stem().string(), true);
        if (!copy_okay) {
            return _u8L(ERROR_MSG_UNABLE_COPY_CONFIG);
        }
    } else {
        assert(!to_install.tag.empty());
        //test if already dowloaded
        // download zip
        boost::filesystem::path temp_dir = GUI::into_path(data_dir()) / "cache" / "vendor" / this->profile.usable_id();
        boost::filesystem::path root_dir = repository_package_cache_path(
            GUI::into_path(data_dir()), RepositoryPackageType::Vendor, this->profile.usable_id(),
            to_install.config_version.to_string(), to_install.slicer_version.to_string());
        if (!boost::filesystem::exists(root_dir)) {
            boost::filesystem::create_directories(temp_dir);
            boost::filesystem::path download_zip_file(data_dir());
            download_zip_file = download_zip_file / "cache" / "vendor" / this->profile.usable_id() / (this->profile.usable_id() + ".zip");
            if (boost::filesystem::exists(download_zip_file)) {
                boost::filesystem::remove(download_zip_file);
            }
            std::string error_message;
            std::atomic_bool cancel = false;
            bool done = false;
            bool res = false;
            if (!api_slot.has_api_request_slot(to_install.url_zip)) {
                return _u8L("Too many requests to github, please try again in an hour");
            }
            Http::get(to_install.url_zip)
                // max 100 mega
                .size_limit(130 * 1024 * 1024)
                .on_error([&](std::string body, std::string error, unsigned http_status) {
                    error_message = GUI::format("Error getting: `%1%`: HTTP %2%, %3%", to_install.url_zip, http_status,
                                                error);
                    BOOST_LOG_TRIVIAL(error) << error_message;
                })
                .on_complete([&](std::string body, unsigned /* http_status */) {
                    FILE *file = boost::nowide::fopen(download_zip_file.string().c_str(), "wb");
                    assert(file != nullptr);
                    if (file == nullptr) {
                        error_message = GUI::format(_u8L("Can't create file at %1%"), download_zip_file.string());
                        res = false;
                        done = true;
                        return;
                    }
                    try {
                        fwrite(body.c_str(), 1, body.size(), file);
                    } catch (const std::exception &e) {
                        error_message = GUI::format(_u8L("Failed to write to file %1% :\n%3%"), download_zip_file,
                                                    e.what());
                        if (file != nullptr) {
                            fclose(file);
                        }
                        res = false;
                        done = true;
                        return;
                    }
                    fclose(file);
                    res = true;
                    done = true;
                })
                // do it here, so we wait the result
                .perform_sync();
            assert(done);
            if (!res) {
                return error_message;
            } else {
                try {
                    const boost::filesystem::path staging = root_dir.parent_path() /
                        boost::filesystem::unique_path("." + this->profile.usable_id() + ".extract-%%%%-%%%%");
                    if (!extract_repository_archive(download_zip_file, staging, error_message)) {
                        boost::filesystem::remove_all(staging);
                        return error_message;
                    }
                    boost::filesystem::path extracted_root = staging;
                    if (!boost::filesystem::exists(extracted_root / "profiles")) {
                        boost::filesystem::directory_iterator entry(staging);
                        const boost::filesystem::directory_iterator end;
                        if (entry == end || !boost::filesystem::is_directory(entry->path())) {
                            boost::filesystem::remove_all(staging);
                            return _u8L("The downloaded vendor archive has no profile directory.");
                        }
                        extracted_root = entry->path();
                        ++entry;
                        if (entry != end || !boost::filesystem::exists(extracted_root / "profiles")) {
                            boost::filesystem::remove_all(staging);
                            return _u8L("The downloaded vendor archive has an invalid root directory.");
                        }
                    }
                    boost::filesystem::rename(extracted_root, root_dir);
                    if (extracted_root != staging)
                        boost::filesystem::remove_all(staging);
                } catch (const boost::filesystem::filesystem_error &) {
                    BOOST_LOG_TRIVIAL(error) << "Fail to upgrade current app by the content of the downloaded zip.";
                    return _u8L("Error while extracting the downloaded zip of the vendor bundle.");
                }
                // copy the file & icons
                assert(boost::filesystem::exists(root_dir));
                bool copy_okay = copy_file_and_icons(root_dir / "profiles", GUI::into_path(data_dir()) / "vendor",
                                                     this->profile.id, true);
                if (!copy_okay) {
                    return _u8L(ERROR_MSG_UNABLE_COPY_CONFIG);
                }
            }
        } else {
            //already downloaded
            // copy the file & icons
            assert(boost::filesystem::exists(root_dir));
            bool copy_okay = copy_file_and_icons(root_dir / "profiles", GUI::into_path(data_dir()) / "vendor",
                                                    this->profile.id, true);
            if (!copy_okay) {
                return _u8L(ERROR_MSG_UNABLE_COPY_CONFIG);
            }
        }
    }
    // refresh this
    VendorProfile vp = VendorProfile::from_ini(GUI::into_path(data_dir()) / "vendor" / (this->profile.id + ".ini"), true);
    this->is_installed = true;
    this->profile = vp;
    this->can_upgrade = !available_profiles.empty() && available_profiles.front().config_version > profile.config_version;
    return "";
}

bool VendorSync::uninstall_vendor_config() {
    // move from vendor to cache
    bool move_okay = copy_file_and_icons(GUI::into_path(data_dir()) / "vendor", GUI::into_path(data_dir()) / "cache" / "vendor" / this->profile.usable_id(), this->profile.usable_id(), false);
    if (!move_okay) {
        return false;
    }
    // unregister from us
    this->is_installed = false;
    this->is_synch = false;
    this->can_upgrade = false;
    this->available_profiles = {};
    return true;
}

bool VendorSync::clear_cache() {
    assert(has_cache);
    // remove cache
    size_t nbdel = boost::filesystem::remove_all(GUI::into_path(data_dir()) / "cache" / "vendor" / this->profile.usable_id());
    return nbdel > 0;
}

void PresetUpdater::show_synch_window(wxWindow *parent,
                                      const wxString &message,
                                      std::function<void(bool)> callback_dialog_closed) {
    {
        std::lock_guard<std::mutex> guard(this->args_for_dialog_mutex);
        assert(args_for_dialog.parent == nullptr);
        args_for_dialog.parent = parent;
        args_for_dialog.message = message;
        args_for_dialog.callback_dialog_closed = callback_dialog_closed;
    }
    if (all_vendors.size() == 0) {
        // they should already be loaded
        this->reload_all_vendors();
    }
    // synch before if not synch
    if (!is_synch) {
        if (synch_process_ongoing) {
            // simple stuff, plz
            std::lock_guard<std::mutex> guard(this->callback_update_preset_mutex);
            if (!synch_process_ongoing) {
                // synch finished while waiting for mutex, all good
                assert(is_synch);
            } else {
                // add our callback
                std::function<void(int)> old_callback_update_preset = callback_update_preset;
                callback_update_preset = [old_callback_update_preset, this](int nb) {
                    old_callback_update_preset(nb);
                    wxCommandEvent *evt = new wxCommandEvent(EVT_CONFIG_UPDATER_SHOW_DIALOG);
                    this->evt_handler->QueueEvent(evt);
                };
                // we'll be called ia the callback
                return;
            }
        } else {
            // asynch call
            sync_async([parent, message, callback_dialog_closed, this](int) {
                wxCommandEvent *evt = new wxCommandEvent(EVT_CONFIG_UPDATER_SHOW_DIALOG);
                this->evt_handler->QueueEvent(evt);
            });
            return;
        }
    }
    else if (synch_process_ongoing) {
        assert(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
    // we didn't add a callback, seems in synch, so call the event directly.
    wxCommandEvent *evt = new wxCommandEvent(EVT_CONFIG_UPDATER_SHOW_DIALOG);
    this->evt_handler->QueueEvent(evt);
}

void PresetUpdater::_show_synch_window_internal() {
    ShowDialogArgs copy;
    {
        std::lock_guard<std::mutex> guard(this->args_for_dialog_mutex);
        if (args_for_dialog.parent == nullptr) {
            assert(false);
            return;
        }
        copy = args_for_dialog;
        args_for_dialog.parent = nullptr;
    }
    GUI::UpdateConfigDialog * dialog = new GUI::UpdateConfigDialog(copy.parent, *this, copy.message);
    int res = dialog->ShowModal();

    if (copy.callback_dialog_closed) {
        copy.callback_dialog_closed(res == wxID_OK);
    }
}

size_t PresetUpdater::count_installed() {
    size_t count = 0;
    std::lock_guard<std::recursive_mutex> guard(this->all_vendors_mutex);
    for (const auto& [id, vendor] : this->all_vendors) {
        if (vendor.is_installed) {
            ++count;
        }
    }
    return count;
}
size_t PresetUpdater::count_available() {
    return this->all_vendors.size();
}

void PresetUpdater::download_new_repo(const std::string &rest_url, std::function<void(bool)> callback_result) {

    if (rest_url.find("https://api.github.com/repos/") != std::string::npos) {
        std::string github_org_repo = rest_url.substr(strlen("https://api.github.com/repos/"));
        assert(github_org_repo.find('/') != std::string::npos);
        if (github_org_repo.find('/') == std::string::npos) {
            assert(false);
            callback_result(false);
            return;
        }
        Http::get(std::string("https://raw.githubusercontent.com/")+github_org_repo+"/refs/heads/main/description.ini")
            .size_limit(1024 * 64 /*64kio, 100tags should use 27ko*/)
            .on_error([callback_result](std::string body, std::string error, unsigned http_status) {
                callback_result(false);
            })
            .on_complete([this, github_org_repo, callback_result](std::string body, unsigned /* http_status */) {
                try {
                    //extract an temp id (it won't be used, there should be one in the description)
                    std::string id = github_org_repo.substr(github_org_repo.find_last_of('/') + 1);
                    // parse it to get id (should be at the end of github_org_repo)
                    boost::property_tree::ptree root;
                    std::stringstream body_stream(body);
                    boost::property_tree::read_ini(body_stream, root);
                    VendorProfile vp = VendorProfile::from_ini(root, id, false);
                    RepositoryDescription description;
                    std::string description_error;
                    if (!parse_repository_description(body, RepositoryPackageType::Vendor, description, description_error) ||
                        description.id != vp.id) {
                        BOOST_LOG_TRIVIAL(warning) << description_error;
                        callback_result(false);
                        return;
                    }
                    // save it
                    boost::filesystem::create_directories(GUI::into_path(data_dir()) / "cache" / "vendor" / vp.usable_id());
                    boost::filesystem::path file_path = GUI::into_path(data_dir()) / "cache" / "vendor" / vp.usable_id() / (vp.usable_id() + ".ini");
                    bool no_error = true;
                    try {
                        std::ofstream file_out;
                        file_out.open(file_path.string().c_str(), std::ofstream::out | std::ofstream::trunc);
                        file_out << body;
                        file_out.close();
                    } catch (const std::exception &err) {
                        BOOST_LOG_TRIVIAL(error) << "PresetUpdater::download_new_repo: Couldn't open "
                                                 << file_path.string().c_str() << " for writing: " << err.what();
                        no_error  =false;
                    }
                    callback_result(no_error);
                } catch (const std::exception &) {
                    callback_result(false);
                    return;
                }

            })
            .perform();
    } else {
        //3rd-party rest api: ask for description
        Http::get(std::string(rest_url+"/description"))
            .size_limit(1024 * 64 /*64kio, 100tags should use 27ko*/)
            .on_error([callback_result](std::string body, std::string error, unsigned http_status) {
                callback_result(false);
            })
            .on_complete([this, rest_url, callback_result](std::string body, unsigned /* http_status */) {
                bool no_error = true;
                try {
                    if (body.empty()) {
                        callback_result(false);
                        return;
                    }
                    // ini or json?
                    bool is_json = body[0] == '{';
                    //extract an temp id (it won't be used, there should be one in the description)
                    std::string id = rest_url.substr(rest_url.find_last_of('/') + 1);
                    // parse it to get id (should be at the end of rest_url)
                    boost::property_tree::ptree root;
                    std::stringstream body_stream(body);
                    VendorProfile vp;
                    if (is_json) {
                        boost::property_tree::read_json(body_stream, root);
                        // from ini should parse the json as well as the ini. (todo: test)
                        vp = VendorProfile::from_ini(root, id, false);
                    } else {
                        boost::property_tree::read_ini(body_stream, root);
                        vp = VendorProfile::from_ini(root, id, false);
                    }
                    if (!is_json) {
                        RepositoryDescription description;
                        std::string description_error;
                        if (!parse_repository_description(body, RepositoryPackageType::Vendor, description, description_error) ||
                            description.id != vp.id) {
                            BOOST_LOG_TRIVIAL(warning) << description_error;
                            no_error = false;
                        }
                    }
                    // save it
                    boost::filesystem::create_directories(GUI::into_path(data_dir()) / "cache" / "vendor" / vp.usable_id());
                    boost::filesystem::path file_path = GUI::into_path(data_dir()) / "cache" / "vendor" / vp.usable_id() / (vp.usable_id() + ".ini");
                    try {
                        std::ofstream file_out;
                        file_out.open(file_path.string().c_str(), std::ofstream::out | std::ofstream::trunc);
                        if (is_json) {
                            //transform into ini
                            file_out << "[vendor]\n";
                            for (boost::property_tree::ptree::value_type &kv : root.get_child("vendor")) {
                                file_out << kv.first;
                                file_out << " = ";
                                file_out << kv.second.data();
                                file_out <<  "\n";
                            }
                            file_out << "\n";
                        } else {
                            file_out << body;
                        }
                        file_out.close();
                    } catch (const std::exception &err) {
                        BOOST_LOG_TRIVIAL(error) << "PresetUpdater::download_new_repo: Couldn't open "
                                                 << file_path.string().c_str() << " for writing: " << err.what();
                        no_error = false;
                    }
                } catch (const std::exception &) {
                    no_error = false;
                }
                callback_result(no_error);
            })
            .perform();
    }
}

void PresetUpdater::uninstall_all_vendors(std::function<void(bool)> callback_result) {
    const GUI::Config::Snapshot* snapshot = take_config_snapshot_report_error(*GUI::wxGetApp().app_config, GUI::Config::Snapshot::SNAPSHOT_UPGRADE,
        _u8L("Before removing all vendor bundles"));
    if (!snapshot) {
        // fail to snapshot, cancel
        BOOST_LOG_TRIVIAL(error) << "Error: can't take snapshot, cancel preset removal.";
        callback_result(false);
        return;
    }
    std::lock_guard<std::recursive_mutex> guard(this->all_vendors_mutex);
    bool at_leat_a_delete = false;
    auto vmap = GUI::wxGetApp().app_config->vendors();
    for (auto& [id, vendor] : this->all_vendors) {
        if (vendor.is_installed) {
            bool res = vendor.uninstall_vendor_config();
            if (res) {
                at_leat_a_delete = true;
                vmap.erase(vendor.profile.id);
            }
        }
    }
    if (at_leat_a_delete) {
        // unregister from appconfig
        GUI::wxGetApp().app_config->set_vendors(vmap);
        GUI::wxGetApp().app_config->save();
        // unregister from current slicer app
        GUI::wxGetApp().preset_bundle->load_installed_printers(*GUI::wxGetApp().app_config);
        GUI::wxGetApp().preset_bundle->load_presets(*GUI::wxGetApp().app_config,
                                                    ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        // update tabs
        GUI::wxGetApp().load_current_presets();
        reload_all_vendors();
        sync_async([callback_result](int nbupdates) { callback_result(true); });
    } else {
        callback_result(true);
    }
}

bool PresetUpdater::has_api_request_slot(const std::string &url) {
    return RepositoryUpdater::has_api_request_slot(url);
}

void PresetUpdater::install_all_vendors(std::function<void(const std::string &)> callback_result) {
    const GUI::Config::Snapshot* snapshot = take_config_snapshot_report_error(*GUI::wxGetApp().app_config, GUI::Config::Snapshot::SNAPSHOT_DOWNGRADE,
        _u8L("Before installing all vendor bundles"));
    if (!snapshot) {
        // fail to snapshot, cancel
        BOOST_LOG_TRIVIAL(error) << "Error: can't take snapshot, cancel preset removal.";
        callback_result(_u8L(ERROR_MSG_UNABLE_SNAPSHOT));
        return;
    }
    std::lock_guard<std::recursive_mutex> guard(this->all_vendors_mutex);
    bool at_leat_an_install = false;
    auto vmap = GUI::wxGetApp().app_config->vendors();
    std::string errors;
    for (auto& [id, vendor] : this->all_vendors) {
        if (!vendor.is_installed && !vendor.available_profiles.empty()) {
            assert(vendor.best);
            std::string res = vendor.install_vendor_config(*vendor.best, *this);
            at_leat_an_install = at_leat_an_install || res.empty();
            errors += errors.empty() ? res : (std::string("\n") + errors);
        }
    }
    if (at_leat_an_install) {
        // the vendor is already in use, update the slicer
        // reload slicer app
        GUI::wxGetApp().preset_bundle->load_installed_printers(*GUI::wxGetApp().app_config);
        GUI::wxGetApp().preset_bundle->load_presets(*GUI::wxGetApp().app_config,
                                                    ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        // update tabs
        GUI::wxGetApp().load_current_presets();
        reload_all_vendors();
        sync_async([callback_result, errors](int nbupdates) { callback_result(errors); });
    } else {
        callback_result(errors);
    }
}

void PresetUpdater::upgrade_all_installed_vendors(std::function<void(const std::string &)> callback_result) {
    const GUI::Config::Snapshot* snapshot = take_config_snapshot_report_error(*GUI::wxGetApp().app_config, GUI::Config::Snapshot::SNAPSHOT_UPGRADE,
        _u8L("Before upgrading all intalled vendor bundles"));
    if (!snapshot) {
        // fail to snapshot, cancel
        BOOST_LOG_TRIVIAL(error) << "Error: can't take snapshot, cancel preset removal.";
        callback_result(_u8L(ERROR_MSG_UNABLE_SNAPSHOT));
        return;
    }
    std::lock_guard<std::recursive_mutex> guard(this->all_vendors_mutex);
    bool at_leat_an_upgrade = false;
    auto vmap = GUI::wxGetApp().app_config->vendors();
    std::string errors;
    for (auto& [id, vendor] : this->all_vendors) {
        if (vendor.is_installed && vendor.can_upgrade) {
            assert(!vendor.available_profiles.empty());
            assert(vendor.best);
            std::string res = vendor.install_vendor_config(*vendor.best, *this);
            if (res.empty()) {
                at_leat_an_upgrade = true;
                // read it with models
                if (vendor.profile.models.empty()) {
                    assert(vendor.is_installed);
                    vendor.profile = VendorProfile::from_ini(GUI::into_path(data_dir()) / "vendor" / (vendor.profile.id + ".ini"), true);
                    assert(!vendor.profile.models.empty());
                }
                // update appconfig : remove unfindable printers
                for (auto printer_it = vmap[vendor.profile.id].begin(); printer_it != vmap[vendor.profile.id].end();) {
                    // search for PrinterModel
                    const VendorProfile::PrinterModel *printer_model = nullptr;
                    for (const VendorProfile::PrinterModel &pm : vendor.profile.models) {
                        if (pm.id == printer_it->first) {
                            printer_model = &pm;
                            break;
                        }
                    }
                    if (printer_model == nullptr) {
                        printer_it = vmap[vendor.profile.id].erase(printer_it);
                    } else {
                        // same for variant
                        for (auto it_variant = printer_it->second.begin(); it_variant != printer_it->second.end();) {
                            bool found_name = false;
                            for (const VendorProfile::PrinterVariant &variant : printer_model->variants) {
                                if (variant.name == *it_variant) {
                                    found_name = true;
                                    break;
                                }
                            }
                            if (!found_name) {
                                it_variant = printer_it->second.erase(it_variant);
                            } else {
                                it_variant++;
                            }
                        }
                        // erase if no variant anymore
                        if (printer_it->second.empty()) {
                            printer_it = vmap[vendor.profile.id].erase(printer_it);
                        } else {
                            // next elt
                            ++printer_it;
                        }
                    }
                }
            } else {
                errors += errors.empty() ? res : (std::string("\n") + res);
            }
        }
    }
    if (at_leat_an_upgrade) {
        // the vendor is already in use, update the slicer
        GUI::wxGetApp().app_config->set_vendors(vmap);
        GUI::wxGetApp().app_config->save();
        // reload slicer app
        GUI::wxGetApp().preset_bundle->load_installed_printers(*GUI::wxGetApp().app_config);
        GUI::wxGetApp().preset_bundle->load_presets(*GUI::wxGetApp().app_config,
                                                    ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        // update tabs
        GUI::wxGetApp().load_current_presets();
        reload_all_vendors();
        sync_async([callback_result, errors](int nbupdates) { callback_result(errors); });
    } else {
        callback_result(errors);
    }

}

} // namespace Slic3r
