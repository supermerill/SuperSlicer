///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2020 - 2023 Tomáš Mészáros @tamasmeszaros, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SL1.hpp"

#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/MarchingSquares.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/MTUtils.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Time.hpp"
#include "libslic3r/Zipper.hpp"

#include "SLAArchiveFormatRegistry.hpp"
#include "SLAArchiveReader.hpp"
#include "ZipperArchiveImport.hpp"

namespace Slic3r {

using ConfMap = std::map<std::string, std::string>;

namespace {

std::string to_ini(const ConfMap &m)
{
    std::string ret;
    for (auto &param : m)
        ret += param.first + " = " + param.second + "\n";

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

void fill_iniconf(ConfMap &m, const SLAPrint &print)
{
    CNumericLocalesSetter locales_setter; // for to_string
    auto &cfg = print.full_print_config();
    m["layerHeight"]    = get_cfg_value(cfg, "layer_height");
    m["expTime"]        = get_cfg_value(cfg, "exposure_time");
    m["expTimeFirst"]   = get_cfg_value(cfg, "initial_exposure_time");
    const std::string mps = get_cfg_value(cfg, "material_print_speed");
    m["expUserProfile"] = mps == "slow" ? "1" : mps == "fast" ? "0" : "2";
    m["materialName"]   = get_cfg_value(cfg, "sla_material_settings_id");
    m["printerModel"]   = get_cfg_value(cfg, "printer_model");
    m["printerVariant"] = get_cfg_value(cfg, "printer_variant");
    m["printerProfile"] = get_cfg_value(cfg, "printer_settings_id");
    m["printProfile"]   = get_cfg_value(cfg, "sla_print_settings_id");
    m["fileCreationTimestamp"] = Utils::utc_timestamp();
    m["prusaSlicerVersion"]    = SLIC3R_BUILD_ID;
    
    SLAPrintStatistics stats = print.print_statistics();
    // Set statistics values to the printer
    
    double used_material = (stats.objects_used_material +
                            stats.support_used_material) / 1000;
    
    int num_fade = print.default_object_config().faded_layers.value;
    num_fade = num_fade >= 0 ? num_fade : 0;
    
    m["usedMaterial"] = std::to_string(used_material);
    m["numFade"]      = std::to_string(num_fade);
    m["numSlow"]      = std::to_string(stats.slow_layers_count);
    m["numFast"]      = std::to_string(stats.fast_layers_count);
    m["printTime"]    = std::to_string(stats.estimated_print_time);

    bool hollow_en = false;
    auto it = print.objects().begin();
    while (!hollow_en && it != print.objects().end())
        hollow_en = (*it++)->config().hollowing_enable;

    m["hollow"] = hollow_en ? "1" : "0";
    
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
        if (!is_banned(key) && (cfg.option(key)->is_enabled() || !cfg.get_option_def(key)->is_optional))
            m[key] = cfg.opt_serialize(key);
    
}

} // namespace

std::unique_ptr<sla::RasterBase> SL1Archive::create_raster() const
{
    sla::Resolution res;
    sla::PixelDim   pxdim;
    std::array<bool, 2>         mirror;

    double w  = m_cfg.display_width.value;
    double h  = m_cfg.display_height.value;
    auto   pw = size_t(m_cfg.display_pixels_x.value);
    auto   ph = size_t(m_cfg.display_pixels_y.value);

    mirror[X] = m_cfg.display_mirror_x.value;
    mirror[Y] = m_cfg.display_mirror_y.value;
    
    auto ro = m_cfg.display_orientation.value;
    sla::RasterBase::Orientation orientation =
        ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait :
                                            sla::RasterBase::roLandscape;
    
    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    res   = sla::Resolution{pw, ph};
    pxdim = sla::PixelDim{w / pw, h / ph};
    sla::RasterBase::Trafo tr{orientation, mirror};

    double gamma = m_cfg.gamma_correction.value;

    return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
}

sla::RasterEncoder SL1Archive::get_encoder() const
{
    return sla::PNGRasterEncoder{};
}

static void write_thumbnail(Zipper &zipper, const ThumbnailData &data)
{
    size_t png_size = 0;

    void  *png_data = tdefl_write_image_to_png_file_in_memory_ex(
         (const void *) data.pixels.data(), data.width, data.height, 4,
         &png_size, MZ_DEFAULT_LEVEL, 1);

    if (png_data != nullptr) {
        zipper.add_entry("thumbnail/thumbnail" + std::to_string(data.width) +
                             "x" + std::to_string(data.height) + ".png",
                         static_cast<const std::uint8_t *>(png_data),
                         png_size);

        mz_free(png_data);
    }
}

void SL1Archive::export_print(Zipper               &zipper,
                              const SLAPrint &print,
                              const ThumbnailsList &thumbnails,
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
        zipper.add_entry("config.ini");
        zipper << to_ini(iniconf);
        zipper.add_entry("prusaslicer.ini");
        zipper << to_ini(slicerconf);

        size_t i = 0;
        for (const sla::EncodedRaster &rst : m_layers) {

            std::string imgname = project + string_printf("%.5d", i++) + "." +
                                  rst.extension();

            zipper.add_entry(imgname.c_str(), rst.data(), rst.size());
        }

        for (const ThumbnailData& data : thumbnails)
            if (data.is_valid())
                write_thumbnail(zipper, data);

        zipper.finalize();
    } catch(std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << e.what();
        // Rethrow the exception
        throw;
    }
}

void SL1Archive::export_print(const std::string     fname,
                              const SLAPrint       &print,
                              const ThumbnailsList &thumbnails,
                              const std::string    &prjname)
{
    Zipper zipper{fname, Zipper::FAST_COMPRESSION};

    export_print(zipper, print, thumbnails, prjname);
}

} // namespace Slic3r

// /////////////////////////////////////////////////////////////////////////////
// Reader implementation
// /////////////////////////////////////////////////////////////////////////////

namespace marchsq {

template<> struct _RasterTraits<Slic3r::png::ImageGreyscale> {
    using Rst = Slic3r::png::ImageGreyscale;

       // The type of pixel cell in the raster
    using ValueType = uint8_t;

       // Value at a given position
    static uint8_t get(const Rst &rst, size_t row, size_t col)
    {
        return rst.get(row, col);
    }

       // Number of rows and cols of the raster
    static size_t rows(const Rst &rst) { return rst.rows; }
    static size_t cols(const Rst &rst) { return rst.cols; }
};

} // namespace marchsq

namespace Slic3r {

template<class Fn> static void foreach_vertex(ExPolygon &poly, Fn &&fn)
{
    for (auto &p : poly.contour.points) fn(p);
    for (auto &h : poly.holes)
        for (auto &p : h.points) fn(p);
}

void invert_raster_trafo(ExPolygons &                  expolys,
                         const sla::RasterBase::Trafo &trafo,
                         coord_t                       width,
                         coord_t                       height)
{
    if (trafo.flipXY) std::swap(height, width);

    for (auto &expoly : expolys) {
        if (trafo.mirror_y)
            foreach_vertex(expoly, [height](Point &p) {p.y() = height - p.y(); });

        if (trafo.mirror_x)
            foreach_vertex(expoly, [width](Point &p) {p.x() = width - p.x(); });

        expoly.translate(-trafo.center_x, -trafo.center_y);

        if (trafo.flipXY)
            foreach_vertex(expoly, [](Point &p) { std::swap(p.x(), p.y()); });

        if ((trafo.mirror_x + trafo.mirror_y + trafo.flipXY) % 2) {
            expoly.contour.reverse();
            for (auto &h : expoly.holes) h.reverse();
        }
    }
}

RasterParams get_raster_params(const DynamicPrintConfig &cfg)
{
    auto *opt_disp_cols = cfg.option<ConfigOptionInt>("display_pixels_x");
    auto *opt_disp_rows = cfg.option<ConfigOptionInt>("display_pixels_y");
    auto *opt_disp_w    = cfg.option<ConfigOptionFloat>("display_width");
    auto *opt_disp_h    = cfg.option<ConfigOptionFloat>("display_height");
    auto *opt_mirror_x  = cfg.option<ConfigOptionBool>("display_mirror_x");
    auto *opt_mirror_y  = cfg.option<ConfigOptionBool>("display_mirror_y");
    auto *opt_orient    = cfg.option<ConfigOptionEnum<SLADisplayOrientation>>("display_orientation");

    if (!opt_disp_cols || !opt_disp_rows || !opt_disp_w || !opt_disp_h ||
        !opt_mirror_x || !opt_mirror_y || !opt_orient)
        throw MissingProfileError("Invalid SL1 / SL1S file");

    RasterParams rstp;

    rstp.px_w = opt_disp_w->value / (opt_disp_cols->value - 1);
    rstp.px_h = opt_disp_h->value / (opt_disp_rows->value - 1);

    rstp.trafo = sla::RasterBase::Trafo{opt_orient->value == sladoLandscape ?
                                            sla::RasterBase::roLandscape :
                                            sla::RasterBase::roPortrait,
                                        {opt_mirror_x->value, opt_mirror_y->value}};

    rstp.height = scale_i(opt_disp_h->value);
    rstp.width  = scale_i(opt_disp_w->value);

    return rstp;
}

namespace {

ExPolygons rings_to_expolygons(const std::vector<marchsq::Ring> &rings,
                               double px_w, double px_h)
{
    auto polys = reserve_vector<ExPolygon>(rings.size());

    for (const marchsq::Ring &ring : rings) {
        Polygon poly; Points &pts = poly.points;
        pts.reserve(ring.size());

        for (const marchsq::Coord &crd : ring)
            pts.emplace_back(scale_i(crd.c * px_w), scale_i(crd.r * px_h));

        polys.emplace_back(poly);
    }

    // TODO: Is a union necessary?
    return union_ex(polys);
}

std::vector<ExPolygons> extract_slices_from_sla_archive(
    ZipperArchive           &arch,
    const RasterParams      &rstp,
    const marchsq::Coord    &win,
    std::function<bool(int)> progr)
{
    std::vector<ExPolygons> slices(arch.entries.size());

    struct Status
    {
        double                                 incr, val, prev;
        bool                                   stop  = false;
        execution::SpinningMutex<ExecutionTBB> mutex = {};
    } st{100. / slices.size(), 0., 0.};

    execution::for_each(
        ex_tbb, size_t(0), arch.entries.size(),
        [&arch, &slices, &st, &rstp, &win, progr](size_t i) {
            // Status indication guarded with the spinlock
            {
                std::lock_guard lck(st.mutex);
                if (st.stop) return;

                st.val += st.incr;
                double curr = std::round(st.val);
                if (curr > st.prev) {
                    st.prev = curr;
                    st.stop = !progr(int(curr));
                }
            }

            png::ImageGreyscale img;
            png::ReadBuf        rb{arch.entries[i].buf.data(),
                            arch.entries[i].buf.size()};
            if (!png::decode_png(rb, img)) return;

            constexpr uint8_t isoval = 128;
            auto              rings = marchsq::execute(img, isoval, win);
            ExPolygons        expolys = rings_to_expolygons(rings, rstp.px_w,
                                                            rstp.px_h);

            // Invert the raster transformations indicated in the profile metadata
            invert_raster_trafo(expolys, rstp.trafo, rstp.width, rstp.height);

            slices[i] = std::move(expolys);
        },
        execution::max_concurrency(ex_tbb));

    if (st.stop) slices = {};

    return slices;
}

} // namespace

ConfigSubstitutions SL1Reader::read(std::vector<ExPolygons> &slices,
                                    DynamicPrintConfig      &profile_out)
{
    Vec2i32 windowsize;

    switch(m_quality)
    {
    case SLAImportQuality::Fast: windowsize = {8, 8}; break;
    case SLAImportQuality::Balanced: windowsize = {4, 4}; break;
    default:
    case SLAImportQuality::Accurate:
        windowsize = {2, 2}; break;
    };

    // Ensure minimum window size for marching squares
    windowsize.x() = std::max(2, windowsize.x());
    windowsize.y() = std::max(2, windowsize.y());

    std::vector<std::string> includes = { "ini", "png"};
    std::vector<std::string> excludes = { "thumbnail" };
    ZipperArchive arch = read_zipper_archive(m_fname, includes, excludes);
    auto [profile_use, config_substitutions] = extract_profile(arch, profile_out);

    RasterParams   rstp = get_raster_params(profile_use);
    marchsq::Coord win  = {windowsize.y(), windowsize.x()};
    slices = extract_slices_from_sla_archive(arch, rstp, win, m_progr);

    return std::move(config_substitutions);
}

ConfigSubstitutions SL1Reader::read(DynamicPrintConfig &out)
{
    ZipperArchive arch = read_zipper_archive(m_fname, {"ini"}, {"png", "thumbnail"});
    return out.load(arch.profile, ForwardCompatibilitySubstitutionRule::Enable);
}

} // namespace Slic3r
