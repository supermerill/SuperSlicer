///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Orchestrator_hpp_
#define slic3r_Orchestrator_hpp_

#include <atomic>
#include <cassert>
#include <map>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_bridge_detector.h"
#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_property.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/MultiPoint.hpp"
#include "libslic3r/Polygon.hpp"

#include "Plugin.hpp"

namespace Slic3r {

class Orchestrator;
class Print;
struct GCodeProcessorResult;
class MultiPoint;
class Polyline;
class Polygon;
class ExPolygon;
class SurfaceCollection;
class ExtrusionEntity;
class PluginStorage;
class DynamicConfig;
namespace ApiClipper { class ClipperShapes; }

} // namespace Slic3r

// Host-side state passed back to callbacks through the opaque C ABI handle.
// It is intentionally not exposed in Api/plugin/c: plugins only see
// plugin_host_context* and must use the callbacks from plugin_run_context.
struct plugin_host_context
{
    Slic3r::Orchestrator *orchestrator = nullptr;
    Slic3r::Print *print = nullptr;
    Slic3r::Plugin *plugin = nullptr;
    slicing_step_t step = STEP_LAYER_HEIGHT;
    size_t object_idx = 0;
    size_t object_count = 0;
    // Generic executions point this at invocation-local state so a reported
    // plugin error can be distinguished from an unrelated cancellation.
    std::atomic_bool *execution_error = nullptr;
};

namespace Slic3r {

struct GenericFacetsAnnotationDefinition
{
    // The key is the stable identity of a facet annotation kind. Project files,
    // Model data and plugin API calls use this key instead of runtime indexes.
    std::string key;

    // User-facing text used by the generic seam-like annotation tool. Built-in GUI
    // classes may replace these with translated strings, while plugins provide
    // plain UTF-8 labels through the C API.
    std::string label;
    std::string enforce_label;
    std::string block_label;
    // Gettext catalog which owns the labels. An empty domain selects the
    // application catalog for definitions created by the host.
    std::string translation_domain;

    // Built-in tools may point to an icon stored in resources/icons. This stays
    // separate from plugin icons so libslic3r does not need to know how wx code
    // stores or rasterizes GUI bitmaps.
    std::string icon_filename;

    // Plugins provide SVG source code directly. The GUI registers this string
    // in BitmapCache under key, so normal wx code can later call
    // get_bmp_bundle(key). The OpenGL toolbar also reads this data directly
    // when it builds its sprite atlas.
    std::string icon_svg;
};

inline GenericFacetsAnnotationDefinition builtin_seam_facets_annotation_definition()
{
    GenericFacetsAnnotationDefinition def;
    def.key = "builtin:seam";
    def.label = "Seam painting";
    def.enforce_label = "Enforce seam";
    def.block_label = "Block seam";
    def.translation_domain = "Slic3r";
    def.icon_filename = "seam.svg";
    return def;
}

// Package loading happens before plugin activation. These values describe the
// package-level result retained for the current process, so updater UI can
// explain why installed code is unavailable without parsing log files.
enum class PluginPackageLoadState
{
    Loading,
    Loaded,
    LoadedWithErrors,
    Failed
};

enum class PluginPackageLoadErrorCode
{
    PackageMissing,
    InvalidPackage,
    LibraryOpenFailed,
    DependencyMissing,
    MissingAbiExport,
    AbiMismatch,
    MissingRegistrationExport,
    RegistrationFailed,
    NoPluginsRegistered,
    PythonRuntimeUnavailable,
    PythonReadFailed,
    PythonCompileFailed,
    PythonImportFailed,
    PythonRegistrationFailed,
    ConfiguredPluginMissing
};

struct PluginPackageLoadIssue
{
    PluginPackageLoadErrorCode code = PluginPackageLoadErrorCode::InvalidPackage;
    std::string detail;
    std::string plugin_id;
    uint32_t system_error = 0;
    uint32_t plugin_abi = 0;
    uint32_t host_abi = 0;
};

struct PluginPackageLoadReport
{
    std::string package_id;
    std::string package_path;
    PluginPackageLoadState state = PluginPackageLoadState::Loading;
    bool allows_no_plugins = false;
    std::vector<std::string> registered_plugin_ids;
    std::vector<PluginPackageLoadIssue> issues;
};

class Orchestrator
{
public:
    struct TranslationCatalog
    {
        std::string domain;
        std::string locale_directory;
        std::string package_root;
    };

    class PluginRegistrationScope
    {
    public:
        PluginRegistrationScope(const PluginRegistrationScope &) = delete;
        PluginRegistrationScope &operator=(const PluginRegistrationScope &) = delete;
        PluginRegistrationScope(PluginRegistrationScope &&other) noexcept;
        PluginRegistrationScope &operator=(PluginRegistrationScope &&other) noexcept;
        ~PluginRegistrationScope();

    private:
        friend class Orchestrator;
        explicit PluginRegistrationScope(Orchestrator *orchestrator) : m_orchestrator(orchestrator) {}

        Orchestrator *m_orchestrator = nullptr;
    };

    struct CustomExtrusionPropertyInfo
    {
        slic3r_property_type type;
        std::string name;
        uint32_t byte_count;
        uint32_t alignment;
    };

    using PropertyInfo = CustomExtrusionPropertyInfo;

    struct GCodeScriptTypeInfo
    {
        gcode_script_type type;
        std::string name;
    };

    struct DynamicStepInfo
    {
        // name is the stable cross-plugin identity. id is only a compact key
        // inside this orchestrator, while invalidates_step tells generated
        // provider selectors which ordinary pipeline result becomes stale.
        slicing_step_t id = STEP_NONE;
        std::string name;
        slicing_step_t invalidates_step = STEP_ANY;
    };

    struct PluginUiFragment
    {
        // One plugin contribution to a .ui file.
        //
        // fragment_id is the de-duplication key inside target_file. This is
        // deliberately separate from plugin_id: alternative plugins may expose
        // the same logical controls and therefore share one fragment id.
        std::string target_file;
        std::string fragment_id;
        std::string content;
        std::string plugin_id;
        std::string exclusive_group;
        int32_t priority = 0;
        uint64_t order = 0;
    };

    struct PluginGuiRule
    {
        raw_gui_rule_action action = RAW_GUI_RULE_ACTION_NONE;
        raw_gui_rule_condition condition = RAW_GUI_RULE_CONDITION_NONE;
        std::string target_key;
        std::string condition_key;
        int32_t target_index = RAW_GUI_RULE_INDEX_ALL;
        int32_t condition_index = RAW_GUI_RULE_INDEX_ALL;
        int32_t condition_int_value = 0;
    };

    struct ConfigOptionOwner
    {
        // Tracks which plugin first introduced a dynamic option key.
        //
        // The owner is not about memory ownership. It is a compatibility rule:
        // another plugin may re-declare the same key only when it is the same
        // plugin or a plugin from the same non-empty exclusive group.
        std::string plugin_id;
        std::string exclusive_group;
    };

    enum class PluginMessageLevel
    {
        Warning,
        Error
    };

    struct PluginMessage
    {
        PluginMessageLevel level = PluginMessageLevel::Warning;
        std::string plugin_id;
        slicing_step_t step = STEP_NONE;
        std::string message;
    };

    static Orchestrator &instance();

    std::vector<Plugin *> registered_plugins() const;
    std::vector<Plugin *> get_all_plugins_for_step(slicing_step_t step) const;
    std::vector<Plugin *> get_active_plugins_for_step(slicing_step_t step) const;
    std::vector<Plugin *> get_current_plugins_for_step(slicing_step_t step) const;
    const Plugin *get_plugin(const std::string &plugin_id) const;
    Plugin *get_plugin(const std::string &plugin_id);
    // Return true only when plugin is one of this orchestrator's owned,
    // registered instances. Public borrowed handles are validated with this
    // check before they are dereferenced or executed.
    bool owns_plugin(const Plugin *plugin) const;
    void add_plugin_to_step(Plugin *plugin, slicing_step_t step);
    bool is_plugin_active(const Plugin *plugin) const;
    bool is_plugin_active(const std::string &plugin_id) const;
    void clear_active_plugins();
    bool set_plugin_active(Plugin *plugin, bool active);
    bool set_plugin_active(const std::string &plugin_id, bool active);
    const std::unordered_set<Plugin *> &active_plugins() const { return m_active_plugins; }

    // Preflight a future active-plugin set before it is written to disk by the
    // plugin configuration dialog.
    //
    // This catches option-key ownership conflicts early enough to show a GUI
    // error without restarting. It intentionally does not replace plugin
    // initialization: the full raw_config_option_def compatibility check still
    // happens when an active plugin actually registers its options.
    bool validate_plugin_activation(const std::vector<std::string> &plugin_ids,
                                    std::string &error_message) const;


    // Register a dynamic print option provided by a plugin.
    //
    // This is the commit point for plugin settings. Compatible duplicate
    // definitions are accepted only inside the same exclusive group, where
    // several alternative plugins intentionally publish the same option.
    option_def_error_code create_new_print_config(const raw_config_option_def *def);

    bool register_plugin(plugin_instance plugin);

    // Register and query plugin-defined service steps. Runtime ids are scoped
    // to this orchestrator; callers exchange the stable name, not the number.
    slicing_step_t register_step(const char *namespaced_name,
                                 slicing_step_t invalidates_step);
    const DynamicStepInfo *step_info(slicing_step_t step) const;
    const DynamicStepInfo *step_info(const char *namespaced_name) const;

    // Execute the normal setup/setup_run/run lifecycle for one plugin. The
    // payload array remains owned by the caller for the duration of this call.
    raw_plugin_execution_status execute_plugin(Plugin &plugin,
                                               Print *print,
                                               const raw_plugin_run_payload *payloads,
                                               uint32_t run_count);

    // Start and complete one installed package load. Loaders add precise
    // issues as they encounter them; successful registrations are associated
    // automatically through plugin_registration_scope().
    void begin_plugin_package_load(const std::string &package_id,
                                   const std::string &package_path,
                                   bool allows_no_plugins = false);
    void report_plugin_package_load_issue(const std::string &package_id,
                                          PluginPackageLoadIssue issue);
    void finish_plugin_package_load(const std::string &package_id);
    void clear_plugin_package_load_reports();
    const PluginPackageLoadReport *plugin_package_load_report(const std::string &package_id) const;
    const std::map<std::string, PluginPackageLoadReport> &plugin_package_load_reports() const
        { return m_plugin_package_load_reports; }

    // The dynamic library loader establishes this scope while it calls a
    // plugin's register_plugin() export. It gives catalog registration a
    // package-relative root without exposing filesystem details through C.
    PluginRegistrationScope plugin_registration_scope(std::string package_root, bool external_plugin);
    int32_t register_translation_catalog(const char *domain, const char *locale_directory);
    const std::vector<TranslationCatalog> &translation_catalogs() const { return m_translation_catalogs; }

    // Add one plugin UI fragment to a target .ui file.
    //
    // target_file + fragment_id is de-duplicated so an installed layout or an
    // alternative plugin can say "this logical fragment is already provided".
    bool add_ui_fragment(const char *target_file,
                         const char *fragment_id,
                         const char *content,
                         int32_t priority);
    std::vector<PluginUiFragment> ui_fragments_for_file(const std::string &target_file) const;
    std::string merged_ui_layout(const std::string &target_file, const std::string &base_content) const;
    std::string merged_ui_layout(const std::string &target_file,
                                 const std::string &base_content,
                                 const std::unordered_set<std::string> &implemented_fragment_ids) const;
    bool add_gui_rule(const raw_gui_rule *rule);
    const std::vector<PluginGuiRule> &gui_rules() const { return m_gui_rules; }

    bridge_detector_instance create_bridge_detector(const bridge_detector_create_input &input);
    void slice(Print &print_to_slice);

    // Export the already-sliced print through the G-code side of the step
    // pipeline. The path_template is resolved with Print::output_filepath()
    // before STEP_GCODE runs, so the selected G-code plugin receives the exact
    // file path it must create.
    std::string export_gcode(Print &print_to_export,
                             const std::string &path_template,
                             GCodeProcessorResult *result,
                             ThumbnailsGeneratorCallback thumbnail_cb = nullptr);

    Orchestrator(const Orchestrator&) = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;
    Orchestrator(Orchestrator&&) = delete;
    Orchestrator& operator=(Orchestrator&&) = delete;

    std::map<Plugin *, PluginStorage> &plugin_storage() { return m_plugin_storage; }
    const std::map<Plugin *, PluginStorage> &plugin_storage() const { return m_plugin_storage; }
    plugin_host_context prepare_plugin_host_context(slicing_step_t step,
                                                    Plugin *plugin,
                                                    Print *print = nullptr);
    plugin_run_context prepare_plugin_run_context(slicing_step_t step,
                                                  Plugin *plugin,
                                                  plugin_host_context *host_context = nullptr);
    // Queue a plugin diagnostic for the GUI thread. The message is copied so C,
    // C++ and Python plugins may pass temporary buffers safely.
    void add_plugin_message(PluginMessageLevel level,
                            const Plugin *plugin,
                            slicing_step_t step,
                            const char *message);
    // Drain queued plugin diagnostics. The Plater uses this to turn plugin
    // warnings and errors into ImGui notifications without opening modal dialogs.
    std::vector<PluginMessage> consume_plugin_messages();
    bool is_plugin_cancelled() const;
    void request_plugin_cancel();
    void reset_plugin_cancel();
    void initialize_plugins();
    slic3r_property_type register_property(const char *namespaced_name,
                                           uint32_t byte_count,
                                           uint32_t alignment);
    const PropertyInfo *property_info(slic3r_property_type type) const;
    const PropertyInfo *property_info(const char *namespaced_name) const;
    extrusion_property_type register_custom_extrusion_property(const char *namespaced_name,
                                                               uint32_t byte_count,
                                                               uint32_t alignment);
    const CustomExtrusionPropertyInfo *custom_extrusion_property_info(extrusion_property_type type) const;
    const CustomExtrusionPropertyInfo *custom_extrusion_property_info(const char *namespaced_name) const;
    gcode_script_type register_gcode_script_type(const char *namespaced_name);
    const GCodeScriptTypeInfo *gcode_script_type_info(gcode_script_type type) const;
    const GCodeScriptTypeInfo *gcode_script_type_info(const char *namespaced_name) const;
    bool register_generic_facets_annotation(GenericFacetsAnnotationDefinition def);
    const std::vector<GenericFacetsAnnotationDefinition> &generic_facets_annotations() const { return m_generic_facets_annotations; }

private:
    struct PluginRegistrationSource
    {
        std::string package_root;
        std::string package_id;
        bool external_plugin = false;
    };

    Orchestrator();
    void end_plugin_registration_scope();
    std::string resolved_translation_domain(const char *requested_domain) const;
    bool is_translation_domain_available(const std::string &domain, const std::string &package_root) const;

    std::vector<std::unique_ptr<Plugin>> m_registered_plugins;
    std::unordered_set<Plugin *> m_active_plugins;
    std::map<slicing_step_t, std::vector<Plugin *>> m_plugins_by_step;
    std::map<Plugin *, PluginStorage> m_plugin_storage;
    std::vector<PluginUiFragment> m_ui_fragments;
    uint64_t m_next_ui_fragment_order { 0 };
    std::vector<PluginGuiRule> m_gui_rules;
    std::map<std::string, ConfigOptionOwner> m_config_option_owners;
    std::vector<TranslationCatalog> m_translation_catalogs;
    std::vector<PluginRegistrationSource> m_plugin_registration_sources;
    std::map<std::string, PluginPackageLoadReport> m_plugin_package_load_reports;
    std::vector<PropertyInfo> m_custom_property_infos;
    slic3r_property_type m_next_custom_property_type { SLIC3R_PROPERTY_TYPE_CUSTOM_BEGIN };
    std::vector<GCodeScriptTypeInfo> m_custom_gcode_script_type_infos;
    gcode_script_type m_next_custom_gcode_script_type{GCODE_SCRIPT_TYPE_CUSTOM_BEGIN};
    std::map<slicing_step_t, DynamicStepInfo> m_dynamic_step_infos;
    uint32_t m_next_dynamic_step { uint32_t(SLICING_STEP_CUSTOM_BEGIN) };
    std::vector<GenericFacetsAnnotationDefinition> m_generic_facets_annotations;
    std::atomic_bool m_plugin_cancel_requested { false };
    std::mutex m_plugin_messages_mutex;
    std::vector<PluginMessage> m_plugin_messages;
    Plugin *m_initializing_plugin { nullptr };
    bool m_initializing_plugin_failed { false };
    std::string m_initializing_plugin_failure;
};

// temporary storage for plugins
template<class T> class StableOwnedVector
{
public:
    using Storage = std::vector<std::unique_ptr<T>>;
    using iterator = typename Storage::iterator;
    using const_iterator = typename Storage::const_iterator;

    template<class... Args> T &emplace_back(Args&&... args) {
        m_items.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
        return *m_items.back();
    }

    T &push_back(std::unique_ptr<T> item) {
        assert(item != nullptr);
        m_items.push_back(std::move(item));
        return *m_items.back();
    }

    iterator begin() { return m_items.begin(); }
    iterator end() { return m_items.end(); }
    const_iterator begin() const { return m_items.begin(); }
    const_iterator end() const { return m_items.end(); }

    iterator erase(iterator it) { return m_items.erase(it); }
    void clear() { m_items.clear(); }

private:
    Storage m_items;
};

class PluginStorage
{
public:
    PluginStorage();
    ~PluginStorage();

    // Stored objects live behind unique_ptr so their addresses stay stable even
    // if the owning vector reallocates. Plugins may keep these handles until
    // storage_free() or clear().
    StableOwnedVector<Polyline> polylines;
    StableOwnedVector<Polygon> polygons;
    StableOwnedVector<ExPolygon> expolygons;
    StableOwnedVector<std::vector<Polyline>> polyline_collections;
    StableOwnedVector<Polygons> polygon_collections;
    StableOwnedVector<std::vector<ExPolygon>> expolygon_collections;
    StableOwnedVector<SurfaceCollection> surface_collections;
    StableOwnedVector<ExtrusionEntity> extrusions;
    StableOwnedVector<DynamicConfig> configs;
    std::vector<std::unique_ptr<ApiClipper::ClipperShapes>> clipper_shapes;
    std::unordered_set<void *> generic_storage;

    void clear();
    size_t size() const;
    bool contains(void *ptr) const;
    bool free(void *ptr);

};

} // namespace Slic3r


#endif // slic3r_Orchestrator_hpp_
