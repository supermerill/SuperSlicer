///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2020 - 2023 Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SLAArchiveReader.hpp"

#include <array>
#include <map>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"

#include "I18N.hpp"
#include "SL1.hpp"
#include "SL1_SVG.hpp"
#include "SLAArchiveFormatRegistry.hpp"

namespace Slic3r {

std::unique_ptr<SLAArchiveReader> SLAArchiveReader::create(
    const std::string       &fname,
    OutputFormat             format_id,
    SLAImportQuality         quality,
    const ProgrFn & progr)
{
    // Create an instance of SLAArchiveReader using the registered archive
    // reader implementations.
    // If format_id is specified and valid, that archive format will be
    // preferred. When format_id is emtpy, the file extension is compared
    // with the advertised extensions of registered readers and the first
    // match will be used.

    std::string ext = boost::filesystem::path(fname).extension().string();
    boost::algorithm::to_lower(ext);

    std::unique_ptr<SLAArchiveReader> ret;

    const std::map<OutputFormat, ArchiveEntry> &registry = registered_sla_archives();

    //auto arch_from = registry.begin();
    //auto arch_to   = registry.end();

    auto arch_it = registry.find(format_id);
    if (arch_it != registry.end()) {
        const ArchiveEntry &archive = arch_it->second;
        if (archive.rdfactoryfn) {
            return archive.rdfactoryfn(fname, quality, progr);
        }
    }

    if (!ext.empty()) {
        if (ext.front() == '.')
            ext.erase(ext.begin());

        for (arch_it = registry.begin(); arch_it != registry.end(); ++arch_it) {
            const ArchiveEntry &archive = arch_it->second;
            if (archive.rdfactoryfn) {
                auto extensions = get_extensions(archive);
                for (const std::string& supportedext : extensions) {
                    if (ext == supportedext) {
                        ret = archive.rdfactoryfn(fname, quality, progr);
                        break;
                    }
                }
            }
        }
    }

    return ret;
}

struct SliceParams { double layerh = 0., initial_layerh = 0.; };

static SliceParams get_slice_params(const DynamicPrintConfig &cfg)
{
    auto *opt_layerh = cfg.option<ConfigOptionFloat>("layer_height");
    auto *opt_init_layerh = cfg.option<ConfigOptionFloat>("initial_layer_height");

    if (!opt_layerh || !opt_init_layerh)
        throw MissingProfileError("Invalid SL1 / SL1S file");

    return SliceParams{opt_layerh->value, opt_init_layerh->value};
}

ConfigSubstitutions import_sla_archive(const std::string       &zipfname,
                                       OutputFormat             format_id,
                                       indexed_triangle_set    &out,
                                       DynamicPrintConfig      &profile,
                                       SLAImportQuality         quality,
                                       const ProgrFn & progr)
{
    ConfigSubstitutions ret;

    if (auto reader = SLAArchiveReader::create(zipfname, format_id, quality, progr)) {
        std::vector<ExPolygons> slices;
        ret = reader->read(slices, profile);

        SliceParams slicp = get_slice_params(profile);

        if (!slices.empty())
            out = slices_to_mesh(slices, 0, slicp.layerh, slicp.initial_layerh);

    } else {
        throw ReaderUnimplementedError("Reader unimplemented");
    }

    return ret;
}

ConfigSubstitutions import_sla_archive(const std::string  &zipfname,
                                       OutputFormat        format_id,
                                       DynamicPrintConfig &out)
{
    ConfigSubstitutions ret;

    if (auto reader = SLAArchiveReader::create(zipfname, format_id)) {
        ret = reader->read(out);
    } else {
        throw ReaderUnimplementedError("Reader unimplemented");
    }

    return ret;
}

} // namespace Slic3r
