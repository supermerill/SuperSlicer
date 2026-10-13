#include "../../src/libslic3r/GitHubBackup.hpp"
#include <iostream>
#include <stdexcept>
using namespace Slic3r::GitHubBackup;
void check(bool ok) { if (!ok) throw std::runtime_error("Backup helper regression"); }
int main()
{
    check(valid_repository("user/my-presets"));
    check(valid_repository("org_1/repo.config"));
    for (const auto *value : {"", "owner", "/repo", "owner/", "a/b/c", "https://github.com/a/b", "a/..", "../b", "a/b?ref=x", "a/b\r\nX: y", "a/b%2fc"})
        check(!valid_repository(value));
    for (const auto *key : {"print_host", "printhost_apikey", "printhost_password", "printhost_client_cert_password", "printhost_user", "octoprint_apikey"})
        check(excluded_option(key));
    for (const auto *key : {"printer_model", "start_gcode", "filament_type", "layer_height"})
        check(!excluded_option(key));
    check(base64("") == "");
    check(base64("f") == "Zg==");
    check(base64("fo") == "Zm8=");
    check(base64("foo") == "Zm9v");
    check(base64("foobar") == "Zm9vYmFy");
    check(base64(std::string("\0\xff\x80", 3)) == "AP+A");
    check(base64("\xc3\xa9") == "w6k=");
    std::cout << "Repository validation, credential exclusion, and Base64 tests passed.\n";
}
