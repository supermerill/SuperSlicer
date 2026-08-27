///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2021 - 2023 Oleksandra Iushchenko @YuSanka, David Kocík @kocikdav, Vojtěch Bubník @bubnikv
///|/ Copyright (c) 2020 Sergey Kovalev @RandoMan70
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "MKS.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <wx/checkbox.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "libslic3r/Config/PrintConfig.hpp"

#include "libslic3r/Updater/Http.hpp"
#include "TCPConsole.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

MKS::MKS(DynamicPrintConfig* config) :
	m_host(config->opt_string("print_host")), m_console_port("8080")
{}

const char* MKS::get_name() const { return "MKS"; }

bool MKS::test(wxString& msg) const
{
	Utils::TCPConsole console(m_host, m_console_port);

	console.enqueue_cmd("M105");
	bool ret = console.run_queue();

	if (!ret)
		msg = wxString::FromUTF8(console.error_message().c_str());

	return ret;
}

wxString MKS::get_test_ok_msg() const
{
	return _(L("Connection to MKS works correctly."));
}

wxString MKS::get_test_failed_msg(wxString& msg) const
{
	return GUI::format_wxstr("%s: %s", _L("Could not connect to MKS"), msg);
}

bool MKS::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
	bool res = true;

	auto upload_cmd = get_upload_url(upload_data.upload_path.string());
	BOOST_LOG_TRIVIAL(info) << boost::format("MKS: Uploading file %1%, filepath: %2%, print: %3%, command: %4%")
		% upload_data.source_path
		% upload_data.upload_path
		% (upload_data.post_action == PrintHostPostUploadAction::StartPrint)
		% upload_cmd;

	auto http = Http::post(std::move(upload_cmd));
	http.set_post_body(upload_data.source_path);

	http.on_complete([&](std::string body, unsigned status) {
		BOOST_LOG_TRIVIAL(debug) << boost::format("MKS: File uploaded: HTTP %1%: %2%") % status % body;

		int err_code = get_err_code_from_body(body);
		if (err_code != 0) {
			BOOST_LOG_TRIVIAL(error) << boost::format("MKS: Request completed but error code was received: %1%") % err_code;
			error_fn(format_error(body, L("Unknown error occured"), 0));
			res = false;
		}
		else if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
			wxString errormsg;
			res = start_print(errormsg, upload_data.upload_path.string());
			if (!res) {
				error_fn(std::move(errormsg));
			}
		}
		})
		.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("MKS: Error uploading file: %1%, HTTP %2%, body: `%3%`") % error % status % body;
			error_fn(format_error(body, error, status));
			res = false;
		})
		.on_progress([&](Http::Progress progress, bool& cancel) {
			prorgess_fn(std::move(progress), cancel);
			if (cancel) {
				// Upload was canceled
				BOOST_LOG_TRIVIAL(info) << "MKS: Upload canceled";
				res = false;
			}
		}).perform_sync();


	return res;
}

std::string MKS::get_upload_url(const std::string& filename) const
{
	return (boost::format("http://%1%/upload?X-Filename=%2%")
		% m_host
		% Http::url_encode(filename)).str();
}

bool MKS::start_print(wxString& msg, const std::string& filename) const
{
	// For some reason printer firmware does not want to respond on gcode commands immediately after file upload.
	// So we just introduce artificial delay to workaround it.
	// TODO: Inspect reasons
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));

	Utils::TCPConsole console(m_host, m_console_port);

	console.enqueue_cmd(std::string("M23 ") + filename);
	console.enqueue_cmd("M24");

	bool ret = console.run_queue();

	if (!ret)
		msg = wxString::FromUTF8(console.error_message().c_str());

	return ret;
}

int MKS::get_err_code_from_body(const std::string& body) const
{
	pt::ptree root;
	std::istringstream iss(body); // wrap returned json to istringstream
	pt::read_json(iss, root);

	return root.get<int>("err", 0);
}

} // Slic3r
