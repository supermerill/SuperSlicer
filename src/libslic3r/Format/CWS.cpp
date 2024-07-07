#include "libslic3r/Format/CWS.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Time.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

namespace Slic3r {

using ConfMap = std::map<std::string, std::string>;
using namespace std::string_literals;

namespace {

std::string to_ini(const ConfMap &m)
{
    std::string ret;
    for (auto &param : m) ret += param.first + " = " + param.second + "\n";
    // this format, at least for the Malyan M100, seems to want this in the config.
    auto _t = m.find("layerHeight"s);
    if (_t != m.cend()) {
        ret += "<SliceHeight>" + _t->second + "</SliceHeight>\n";
    }

    return ret;
}

std::string get_cfg_value(const DynamicPrintConfig &cfg, const std::string &key)
{
    std::string ret;

    if (cfg.has(key)) {
        auto opt = cfg.option(key);
        if (opt) ret = opt->serialize();
    }

    return ret;
}

/// Set up list of configuration options in the default slicing.
void fill_iniconf(ConfMap &m, const SLAPrint &print)
{
    auto &cfg = print.full_print_config();
    m["layerHeight"]    = get_cfg_value(cfg, "layer_height");
    m["expTime"]        = get_cfg_value(cfg, "exposure_time");
    m["expTimeFirst"]   = get_cfg_value(cfg, "initial_exposure_time");
    m["materialName"]   = get_cfg_value(cfg, "sla_material_settings_id");
    m["printerModel"]   = get_cfg_value(cfg, "printer_model");
    m["printerVariant"] = get_cfg_value(cfg, "printer_variant");
    m["printerProfile"] = get_cfg_value(cfg, "printer_settings_id");
    m["printProfile"]   = get_cfg_value(cfg, "sla_print_settings_id");
    m["fileCreationTimestamp"] = Utils::utc_timestamp();
    m["slicerName"]     = SLIC3R_APP_NAME;
    m["slicerVersion"]  = SLIC3R_VERSION_FULL;

    SLAPrintStatistics stats = print.print_statistics();
    // Set statistics values to the printer

    double used_material = (stats.objects_used_material +
                            stats.support_used_material) / 1000;

    int num_fade = print.default_object_config().faded_layers.get_int();
    num_fade = num_fade >= 0 ? num_fade : 0;

    m["usedMaterial"] = std::to_string(used_material);
    m["numFade"]      = std::to_string(num_fade);
    m["numSlow"]      = std::to_string(stats.slow_layers_count);
    m["numFast"]      = std::to_string(stats.fast_layers_count);
    m["printTime"]    = std::to_string(stats.estimated_print_time);

    m["action"] = "print";
}

void fill_slicerconf(ConfMap &m, const SLAPrint &print)
{
    using namespace std::literals::string_view_literals;

    // Sorted list of config keys, which shall not be stored into the ini.
    static constexpr auto banned_keys = {
        "compatible_printers"sv,
        "compatible_prints"sv,
        //FIXME The print host keys should not be exported to full_print_config anymore. The following keys may likely be removed.
        "print_host"sv,
        "printhost_apikey"sv,
        "printhost_cafile"sv
    };

    assert(std::is_sorted(banned_keys.begin(), banned_keys.end()));
    auto is_banned = [](const std::string &key) {
        return std::binary_search(banned_keys.begin(), banned_keys.end(), key);
    };

    auto &cfg = print.full_print_config();
    for (const std::string &key : cfg.keys())
        if (! is_banned(key) && ! cfg.option(key)->is_nil())
            m[key] = cfg.opt_serialize(key);

}

} // namespace


void MaskedCWSArchive::export_print(Zipper& zipper,
                              const SLAPrint &print,
                              const std::string &prjname)
{
    std::string project =
        prjname.empty() ?
            boost::filesystem::path(zipper.get_filename()).stem().string() :
            prjname;

    ConfMap iniconf, slicerconf;
    fill_iniconf(iniconf, print);

    iniconf["jobDir"] = project;

    fill_slicerconf(slicerconf, print);

    try {
        zipper.add_entry("default.slicing");
        zipper << to_ini(iniconf);

        zipper.add_entry("slicer.ini");
        zipper << to_ini(slicerconf);

        size_t i = 0;
        for (const sla::EncodedRaster &rst : m_layers) {

            std::string imgname = project + string_printf("%.5d", i++) + "." +
                                  rst.extension();

            zipper.add_entry(imgname.c_str(), rst.data(), rst.size());
        }
    } catch(std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        // Rethrow the exception
        throw;
    }
}
} // namespace Slic3r
