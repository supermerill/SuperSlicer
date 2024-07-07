#ifndef ARCHIVETRAITS_HPP
#define ARCHIVETRAITS_HPP

#include <string>

#include "libslic3r/Zipper.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Format/SLAArchive.hpp"

namespace Slic3r {

class SL1Archive: public SLAAbstractArchive {
    SLAPrinterConfig m_cfg;

protected:
    SLAPrinterConfig& config() override { return m_cfg; }
    const SLAPrinterConfig& config() const override { return m_cfg; }

public:

    SL1Archive() = default;
    explicit SL1Archive(const SLAPrinterConfig &cfg): m_cfg(cfg) {}
    explicit SL1Archive(SLAPrinterConfig &&cfg): m_cfg(std::move(cfg)) {}

    void export_print(Zipper &zipper, const SLAPrint &print, const std::string &projectname = "") override;
};

ConfigSubstitutions import_sla_archive(const std::string &zipfname, DynamicPrintConfig &out);

ConfigSubstitutions import_sla_archive(
    const std::string &      zipfname,
    Vec2i32                  windowsize,
    indexed_triangle_set &   out,
    DynamicPrintConfig &     profile,
    std::function<bool(int)> progr = [](int) { return true; });

inline ConfigSubstitutions import_sla_archive(
    const std::string &      zipfname,
    Vec2i32                  windowsize,
    indexed_triangle_set &   out,
    std::function<bool(int)> progr = [](int) { return true; })
{
    DynamicPrintConfig profile;
    return import_sla_archive(zipfname, windowsize, out, profile, progr);
}

class MissingProfileError : public RuntimeError { using RuntimeError::RuntimeError; };

} // namespace Slic3r::sla

#endif // ARCHIVETRAITS_HPP
