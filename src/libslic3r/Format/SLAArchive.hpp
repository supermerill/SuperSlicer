#ifndef slic3r_FORMAT_SLACOMMON_HPP
#define slic3r_FORMAT_SLACOMMON_HPP

#include <string>

#include "libslic3r/Zipper.hpp"
#include "libslic3r/SLAPrint.hpp"

namespace Slic3r {

/// Common abstract base class for SLA archive formats.
/// Partial refactor from Slic3r::SL1Archive
class SLAAbstractArchive: public SLAArchive {
protected:
    virtual SLAPrinterConfig& config() = 0;
    virtual const SLAPrinterConfig& config() const = 0;

    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;
public:
    SLAAbstractArchive() = default;

    /// Actually perform the export.
    virtual void export_print(Zipper &zipper, const SLAPrint &print, const std::string &projectname = "") = 0;

    /// Export to a file. Virtual for overriding functions to change how the raster is assembled.
    virtual void export_print(const std::string &fname, const SLAPrint &print, const std::string &projectname = "")
    {
        Zipper zipper(fname);
        export_print(zipper, print, projectname);
    }

    void apply(const SLAPrinterConfig &cfg) override
    {
        auto diff = this->config().diff(cfg);
        if (!diff.empty()) {
            this->config().apply_only(cfg, diff);
            m_layers = {};
        }
    }

};

} // namespace Slic3r

#endif // slic3r_FORMAT_SLACOMMON_HPP
