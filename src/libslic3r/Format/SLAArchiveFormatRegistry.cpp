///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SLAArchiveFormatRegistry.hpp"

#include <memory>
#include <mutex>
#include <set>

#include "AnycubicSLA.hpp"
#include "Config/ConfigOption.hpp"
#include "CWS.hpp"
#include "I18N.hpp"
#include "SL1.hpp"
#include "SL1_SVG.hpp"

namespace Slic3r {

class Registry {
    static std::unique_ptr<Registry> registry;

    std::map<OutputFormat, ArchiveEntry> entries;

    Registry ()
    {
        entries.emplace(OutputFormat::ofSL1, ArchiveEntry(
                OutputFormat::ofSL1,
                "SL1",                      // id
                L("SL1 archive"),    // description
                "sl1",                      // main extension
                {"sl1s", "zip"},            // extension aliases

                // Writer factory
                [] (const auto &cfg) { return std::make_unique<SL1Archive>(cfg); },

                // Reader factory
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            ));
            entries.emplace(OutputFormat::ofSL1_SVG, ArchiveEntry(
                OutputFormat::ofSL1_SVG,
                "SL1SVG",
                L("SL1 SVG archive"),
                "sl1_svg",
                {"zip"},
                [] (const auto &cfg) { return std::make_unique<SL1_SVGArchive>(cfg); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1_SVGReader>(fname, quality, progr);
                }
            ));
            entries.emplace(OutputFormat::ofAnycubicMono, anycubic_sla_format(OutputFormat::ofAnycubicMono, "pwmo", "Photon Mono"));
            entries.emplace(OutputFormat::ofAnycubicMonoX, anycubic_sla_format(OutputFormat::ofAnycubicMonoX, "pwmx", "Photon Mono X"));
            entries.emplace(OutputFormat::ofAnycubicMonoSE, anycubic_sla_format(OutputFormat::ofAnycubicMonoSE, "pwms", "Photon Mono SE"));
            entries.emplace(OutputFormat::ofMaskedCWS, masked_cws_format());

            /**
                // Supports only ANYCUBIC_SLA_VERSION_1
                anycubic_sla_format_versioned("pws", "Photon / Photon S", ANYCUBIC_SLA_VERSION_1),
                anycubic_sla_format_versioned("pw0", "Photon Zero", ANYCUBIC_SLA_VERSION_1),
                anycubic_sla_format_versioned("pwx", "Photon X", ANYCUBIC_SLA_VERSION_1),

                // Supports ANYCUBIC_SLA_VERSION_1 and ANYCUBIC_SLA_VERSION_515
                anycubic_sla_format_versioned("pwmo", "Photon Mono", ANYCUBIC_SLA_VERSION_1),
                anycubic_sla_format_versioned("pwms", "Photon Mono SE", ANYCUBIC_SLA_VERSION_1),
                anycubic_sla_format_versioned("dlp", "Photon Ultra", ANYCUBIC_SLA_VERSION_1),
                anycubic_sla_format_versioned("pwmx", "Photon Mono X", ANYCUBIC_SLA_VERSION_1),
                anycubic_sla_format_versioned("pmsq", "Photon Mono SQ", ANYCUBIC_SLA_VERSION_1),

                // Supports ANYCUBIC_SLA_VERSION_515 and ANYCUBIC_SLA_VERSION_516
                anycubic_sla_format_versioned("pwma", "Photon Mono 4K", ANYCUBIC_SLA_VERSION_515),
                anycubic_sla_format_versioned("pm3",  "Photon M3", ANYCUBIC_SLA_VERSION_515),
                anycubic_sla_format_versioned("pm3m", "Photon M3 Max", ANYCUBIC_SLA_VERSION_515),

                // Supports NYCUBIC_SLA_VERSION_515 and ANYCUBIC_SLA_VERSION_516 and ANYCUBIC_SLA_VERSION_517
                anycubic_sla_format_versioned("pwmb", "Photon Mono X 6K / Photon M3 Plus", ANYCUBIC_SLA_VERSION_515),
                anycubic_sla_format_versioned("dl2p", "Photon Photon D2", ANYCUBIC_SLA_VERSION_515),
                anycubic_sla_format_versioned("pmx2", "Photon Mono X2", ANYCUBIC_SLA_VERSION_515),
                anycubic_sla_format_versioned("pm3r", "Photon M3 Premium", ANYCUBIC_SLA_VERSION_515),
            */
    }

public:

    static const Registry& get_instance()
    {
        if (!registry)
            registry.reset(new Registry());

        return *registry;
    }

    static const std::map<OutputFormat, ArchiveEntry>& get()
    {
        return get_instance().entries;
    }
};

std::unique_ptr<Registry> Registry::registry = nullptr;

const std::map<OutputFormat, ArchiveEntry>& registered_sla_archives()
{
    return Registry::get();
}

std::vector<std::string> get_extensions(const ArchiveEntry &entry)
{
    auto ret = reserve_vector<std::string>(entry.ext_aliases.size() + 1);

    ret.emplace_back(entry.ext);
    for (const char *alias : entry.ext_aliases)
        ret.emplace_back(alias);

    return ret;
}

ArchiveWriterFactory get_writer_factory(OutputFormat formatid)
{
    ArchiveWriterFactory ret;
    auto entry = Registry::get().find(formatid);
    if (entry != Registry::get().end())
        ret = entry->second.wrfactoryfn;

    return ret;
}

ArchiveReaderFactory get_reader_factory(OutputFormat formatid)
{

    ArchiveReaderFactory ret;
    auto entry = Registry::get().find(formatid);
    if (entry != Registry::get().end())
        ret = entry->second.rdfactoryfn;

    return ret;
}

const char *get_default_extension(OutputFormat formatid)
{
    static constexpr const char *Empty = "";

    const char * ret = Empty;

    auto entry = Registry::get().find(formatid);
    if (entry != Registry::get().end())
        ret = entry->second.ext;

    return ret;
}

const ArchiveEntry * get_archive_entry(OutputFormat formatid)
{
    const ArchiveEntry *ret = nullptr;

    auto entry = Registry::get().find(formatid);
    if (entry != Registry::get().end())
        ret = &(entry->second);

    return ret;
}

} // namespace Slic3r::sla
