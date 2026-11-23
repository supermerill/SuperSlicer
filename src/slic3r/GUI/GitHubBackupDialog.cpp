// SuperSlicer is released under the terms of the AGPLv3 or higher.
#include "GitHubBackupDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "GUI.hpp"
#include "libslic3r/GitHubBackup.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/Http.hpp"
#include <nlohmann/json.hpp>
#include <boost/nowide/fstream.hpp>
#include <wx/filename.h>
#include <wx/textdlg.h>
#include <wx/progdlg.h>
#include <wx/filefn.h>
#include <wx/msgdlg.h>
#include <future>
#include <chrono>
#include <stdexcept>

namespace Slic3r { namespace GUI {
namespace {
struct TemporaryFile {
    wxString path = wxFileName::CreateTempFileName("superslicer-backup-");
    TemporaryFile() { if (path.empty()) throw std::runtime_error("Cannot create temporary backup file."); }
    ~TemporaryFile() { wxRemoveFile(path); }
    std::string name() const { return std::string(path.ToUTF8()); }
};

struct Response { unsigned status = 0; std::string body; };
Response request(Http &http, const std::string &token)
{
    Response result;
    http.sensitive().header("Authorization", "Bearer " + token)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28")
        .header("User-Agent", "SuperSlicer")
        .timeout_connect(15).timeout_max(60)
        .on_complete([&](std::string body, unsigned status) { result = {status, std::move(body)}; })
        // Do not echo server responses or transport diagnostics containing credentials.
        .on_error([&](std::string, std::string, unsigned status) { result.status = status; })
        .perform_sync();
    return result;
}
void require_success(const Response &response)
{
    if (response.status == 200 || response.status == 201) return;
    if (response.status == 0) throw std::runtime_error("GitHub could not be reached. Check your connection. If this happened during upload, check the repository before retrying.");
    if (response.status == 401 || response.status == 403) throw std::runtime_error("GitHub denied access. Check your token, repository Contents write permission, and API rate limit.");
    if (response.status == 404) throw std::runtime_error("Repository or branch not found, or the token cannot access it.");
    if (response.status == 409 || response.status == 422) throw std::runtime_error("GitHub rejected the update. Check branch protection and retry after checking the repository for conflicting changes.");
    throw std::runtime_error("GitHub request failed (HTTP " + std::to_string(response.status) + ").");
}
void upload(const std::string &repository, const std::string &token, const std::string &bundle)
{
    const std::string root = "https://api.github.com/repos/" + repository;
    auto metadata_request = Http::get(root);
    const auto metadata = request(metadata_request, token);
    require_success(metadata);
    const auto branch = nlohmann::json::parse(metadata.body).at("default_branch").get<std::string>();
    const std::string url = root + "/contents/SuperSlicer_config_bundle.ini";
    auto get = Http::get(url + "?ref=" + Http::url_encode(branch));
    const auto existing = request(get, token);
    nlohmann::json payload = {{"message", "Back up SuperSlicer presets"},
        {"branch", branch}, {"content", GitHubBackup::base64(bundle)}};
    if (existing.status == 200) {
        const auto file = nlohmann::json::parse(existing.body);
        if (file.at("type") != "file") throw std::runtime_error("The backup path is not a regular file.");
        payload["sha"] = file.at("sha").get<std::string>();
    } else if (existing.status != 404) require_success(existing);
    TemporaryFile body;
    {
        boost::nowide::ofstream output(body.name(), std::ios::binary | std::ios::trunc);
        output << payload.dump();
        output.close();
        if (!output) throw std::runtime_error("Cannot write backup request.");
    }
    auto put = Http::put(url);
    put.header("Content-Type", "application/json").set_put_body(boost::filesystem::path(body.name()));
    require_success(request(put, token));
}
}

void show_github_backup_dialog(wxWindow *parent)
{
    if (!wxGetApp().check_and_save_current_preset_changes(_L("Backing up presets to GitHub"),
        _L("Unsaved preset changes will not be included in the backup."), false, true)) return;
    wxTextEntryDialog repository_dialog(parent,
        _L("Enter an existing GitHub repository as owner/repository. A private repository is recommended.\n\nThis uploads saved user presets to SuperSlicer_config_bundle.ini on the default branch.\nPrinter connections, UI preferences, and external files are excluded.\nReview custom G-code for private information before uploading."),
        _L("Back Up Presets to GitHub"), wxString::FromUTF8(wxGetApp().app_config->get("github_backup_repository").c_str()));
    if (repository_dialog.ShowModal() != wxID_OK) return;
    const std::string repository = std::string(repository_dialog.GetValue().Trim().Trim(false).ToUTF8());
    if (!GitHubBackup::valid_repository(repository)) {
        show_error(parent, _L("Enter a repository in owner/repository format, without a URL."));
        return;
    }
    wxPasswordEntryDialog token_dialog(parent,
        _L("Enter a GitHub personal access token with Contents read/write permission for this repository.\nThe token is used for this backup only and is not saved."), _L("GitHub Access Token"));
    if (token_dialog.ShowModal() != wxID_OK) return;
    const std::string token = std::string(token_dialog.GetValue().ToUTF8());
    token_dialog.SetValue(wxEmptyString);
    if (token.empty() || token.find_first_of("\r\n") != std::string::npos) {
        show_error(parent, _L("Enter a valid GitHub access token."));
        return;
    }
    try {
        TemporaryFile file;
        wxGetApp().preset_bundle->export_configbundle(file.name(), false, false, true);
        boost::nowide::ifstream input(file.name(), std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read exported presets.");
        const std::string bundle{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad() || bundle.empty()) throw std::runtime_error("Cannot read exported presets.");
        if (bundle.size() > 1024 * 1024) throw std::runtime_error("The preset bundle exceeds the 1 MiB backup limit.");
        auto future = std::async(std::launch::async, [repository, token, bundle] { upload(repository, token, bundle); });
        {
            wxProgressDialog progress(_L("GitHub Backup"), _L("Uploading presets to GitHub..."), 100, parent,
                wxPD_APP_MODAL | wxPD_AUTO_HIDE);
            while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
                progress.Pulse();
            future.get();
        }
        wxGetApp().app_config->set("github_backup_repository", repository);
        wxMessageBox(_L("Presets backed up successfully to:") + "\nhttps://github.com/" + wxString::FromUTF8(repository.c_str()) +
            "\n\n" + _L("To restore, download SuperSlicer_config_bundle.ini and use File > Import > Import Config Bundle."),
            _L("GitHub Backup"), wxOK | wxICON_INFORMATION, parent);
    } catch (const std::exception &error) {
        show_error(parent, error.what());
    }
}
}}
