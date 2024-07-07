#ifndef slic3r_Format_3mf_hpp_
#define slic3r_Format_3mf_hpp_

namespace Slic3r {

    /* The format for saving the SLA points was changing in the past. This enum holds the latest version that is being currently used.
     * Examples of the Slic3r_PE_sla_support_points.txt for historically used versions:

     *  version 0 : object_id=1|-12.055421 -2.658771 10.000000
                    object_id=2|-14.051745 -3.570338 5.000000
        // no header and x,y,z positions of the points)

     * version 1 :  ThreeMF_support_points_version=1
                    object_id=1|-12.055421 -2.658771 10.000000 0.4 0.0
                    object_id=2|-14.051745 -3.570338 5.000000 0.6 1.0
        // introduced header with version number; x,y,z,head_size,is_new_island)
    */

    enum {
        support_points_format_version = 1
    };

    enum {
        drain_holes_format_version = 1
    };

    class Model;
    struct ConfigSubstitutionContext;
    class DynamicPrintConfig;
    struct ThumbnailData;

    // Returns true if the 3mf file with the given filename is a PrusaSlicer project file (i.e. if it contains a config).
    extern bool is_project_3mf(const std::string& filename);

    // Load the content of a 3mf file into the given model and preset bundle.
    extern bool load_3mf(const char* path, DynamicPrintConfig& config, ConfigSubstitutionContext& config_substitutions, Model* model, bool check_version);

    struct OptionStore3mf {
        bool fullpath_sources = true;
        bool zip64 = true;
        bool export_config = true;
        bool export_modifiers = true;
        const ThumbnailData* thumbnail_data = nullptr;
        OptionStore3mf& set_fullpath_sources(bool use_fullpath_sources) { fullpath_sources = use_fullpath_sources; return *this; }
        OptionStore3mf& set_zip64(bool use_zip64) { zip64 = use_zip64; return *this; }
        OptionStore3mf& set_export_config(bool use_export_config) { export_config = use_export_config; return *this; }
        OptionStore3mf& set_export_modifiers(bool use_export_modifiers) { export_modifiers = use_export_modifiers; return *this; }
        OptionStore3mf& set_thumbnail_data(const ThumbnailData* thumbnail) { thumbnail_data = thumbnail; return *this; }
    };

    // Save the given model and the config data contained in the given Print into a 3mf file.
    // The model could be modified during the export process if meshes are not repaired or have no shared vertices
    extern bool store_3mf(const char* path, Model* model, const DynamicPrintConfig* config, OptionStore3mf options = OptionStore3mf{});

} // namespace Slic3r

#endif /* slic3r_Format_3mf_hpp_ */
