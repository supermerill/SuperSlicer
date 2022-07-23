#ifndef slic3r_OctoPrint_hpp_
#define slic3r_OctoPrint_hpp_

#include <string>
#include <wx/string.h>
#include <wx/arrstr.h>
#include <boost/optional.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"


namespace Slic3r {

class DynamicPrintConfig;
class Http;

class OctoPrint : public PrintHost
{
public:
    OctoPrint(DynamicPrintConfig *config);
    ~OctoPrint() override = default;

    const char* get_name() const override;

    bool test(wxString &curl_msg) const override;
    wxString get_test_failed_msg (wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn) const override;
    bool has_auto_discovery() const override { return true; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }
    const std::string& get_apikey() const { return m_apikey; }
    const std::string& get_cafile() const { return m_cafile; }
    const std::string& get_client_cert() const { return m_client_cert; }
    const std::string& get_client_cert_password() const { return m_client_cert_password; }
    const bool get_client_cert_enabled() const { return m_client_cert_enabled; }


protected:
    virtual bool validate_version_text(const boost::optional<std::string> &version_text) const;
    virtual void set_http_send(Http& request, const PrintHostUpload& upload_data) const;

private:
    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;
    std::string m_client_cert;
    std::string m_client_cert_password;
    bool m_client_cert_enabled;

    virtual void set_auth(Http &http) const;
    std::string make_url(const std::string &path) const;
};

class MiniDeltaLCD : public OctoPrint
{
public:
    MiniDeltaLCD(DynamicPrintConfig* config);
    ~MiniDeltaLCD() override = default;

    const char* get_name() const override;

    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    PrintHostPostUploadActions get_post_upload_actions() const override { return {}; }

protected:
    bool validate_version_text(const boost::optional<std::string>& version_text) const override;
    void set_http_send(Http& request, const PrintHostUpload& upload_data) const override;

};

class SL1Host: public OctoPrint
{
public:
    SL1Host(DynamicPrintConfig *config);
    ~SL1Host() override = default;

    const char* get_name() const override;

    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    PrintHostPostUploadActions get_post_upload_actions() const override { return {}; }

protected:
    bool validate_version_text(const boost::optional<std::string> &version_text) const override;

private:
    void set_auth(Http &http) const override;

    // Host authorization type.
    AuthorizationType m_authorization_type;
    // username and password for HTTP Digest Authentization (RFC RFC2617)
    std::string m_username;
    std::string m_password;
};

class PrusaLink : public OctoPrint
{
public:
    PrusaLink(DynamicPrintConfig* config);
    ~PrusaLink() override = default;

    const char* get_name() const override;

    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }

protected:
    bool validate_version_text(const boost::optional<std::string>& version_text) const override;

private:
    void set_auth(Http& http) const override;

    // Host authorization type.
    AuthorizationType m_authorization_type;
    // username and password for HTTP Digest Authentization (RFC RFC2617)
    std::string m_username;
    std::string m_password;
};

}

#endif
