///|/ Copyright (c) Superslicer 2021 - 2024 Durand Rémi @supermerill
///|/ Copyright (c) 2021 Alexander Bachler Jansson
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "MPMDv2.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <wx/progdlg.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "Http.hpp"
#include "../GUI/format.hpp"


namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

MPMDv2::MPMDv2(DynamicPrintConfig *config) :
    host(config->opt_string("print_host"))
{}

const char* MPMDv2::get_name() const { return "MPMDv2"; }

bool MPMDv2::test(wxString &msg) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure

    const char *name = get_name();

    bool res = true;
    auto url = make_url("api/version");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;

            try {
                std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                if (! ptree.get_optional<std::string>("api")) {
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res = validate_version_text(text);
                if (! res) {
                    msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : "MiniDeltaLCD"));
                }
            }
            catch (const std::exception &) {
                res = false;
                msg = "Could not parse server response";
            }
        })
        .perform_sync();

    return res;
}

bool MPMDv2::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn,  InfoFn info_fn) const
{
    const char *name = get_name();

    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();

    wxString test_msg;
    if (! test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    bool res = true;

    auto url = make_url("api/files/local");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%")
        % name
        % upload_data.source_path
        % url
        % upload_filename.string()
        % upload_parent_path.string()
        % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "Start print" : upload_data.post_action == PrintHostPostUploadAction::StartSimulation ? "Start simulation" : "no");

    auto http = Http::post(std::move(url));
    http.form_add("path", upload_parent_path.string())
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << get_name() << ": Upload canceled";
                res = false;
            }
        })
        .perform_sync();

    return res;
}

bool MPMDv2::validate_version_text(const boost::optional<std::string> &version_text) const
{
    return version_text ? boost::starts_with(*version_text, "MiniDeltaLCD") : true;
}
wxString MPMDv2::get_test_ok_msg () const
{
    return GUI::format_wxstr(_L("Connection to %1% works correctly."), get_name());
}

wxString MPMDv2::get_test_failed_msg (wxString &msg) const
{
    return GUI::format_wxstr("%s: %s\n\n%s",
        (boost::format(_u8L("Could not connect to %s")) % get_name()),
        std::string(msg.ToUTF8()),
        _u8L("Note: MiniDeltaLCD version at least 1.0 is required.")
        );
}

std::string MPMDv2::make_url(const std::string &path) const
{
    if (host.find("http://") == 0 || host.find("https://") == 0) {
        if (host.back() == '/') {
            return (boost::format("%1%%2%") % host % path).str();
        } else {
            return (boost::format("%1%/%2%") % host % path).str();
        }
    } else {
        return (boost::format("http://%1%/%2%") % host % path).str();
    }
}

}
