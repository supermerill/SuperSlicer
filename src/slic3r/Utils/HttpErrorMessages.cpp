///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This file converts technical TLS initialization data into the localized
// wording shown by the GUI. The HTTP transport intentionally never includes it.

#include "HttpErrorMessages.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

namespace Slic3r {
namespace GUI {

// Preserves the existing GUI wording while leaving the transport result useful
// to non-GUI callers that cannot load translations.
std::string format_tls_initialization_message(const Http::TlsInitializationResult &result)
{
    std::string message;

    if (result.certificate_store_status == Http::TlsInitializationResult::CertificateStoreStatus::StoreNotDetected) {
        message = _u8L("Could not detect system SSL certificate store. "
                       "The slicer will be unable to establish secure "
                       "network connections.");
    } else if (result.certificate_store_status == Http::TlsInitializationResult::CertificateStoreStatus::FallbackStoreDetected) {
        message = format(_u8L("The slicer detected system SSL certificate store in: %1%"),
                         result.certificate_store_path);
    }

    if (result.certificate_store_status != Http::TlsInitializationResult::CertificateStoreStatus::NotChecked) {
        message += "\n" + format(_u8L("To specify the system certificate store manually, please "
                                       "set the %1% environment variable to the correct CA bundle "
                                       "and restart the application."),
                                 result.certificate_store_environment_variable);
    }

    if (!result.curl_error.empty())
        message += _u8L("CURL init has failed. The slicer will be unable to establish "
                        "network connections. See logs for additional details.");

    return message;
}

} // namespace GUI
} // namespace Slic3r
