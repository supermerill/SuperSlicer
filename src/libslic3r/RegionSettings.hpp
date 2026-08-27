///|/ Copyright (c) SuperSlicer 2025 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_RegionSettings_hpp_
#define slic3r_RegionSettings_hpp_

#include <cassert>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "BoundingBox.hpp"
#include "DataTreeFwd.hpp"
#include "ExPolygon.hpp"
#include "libslic3r.h"
#include "Config/FFFPrintConfig.hpp"

namespace Slic3r {

class RegionSettings
{
public:
    struct ClipExpoly
    {
        // can be empty if only one region (it means there is no clip to do, evrythign can be kept)
        ExPolygons expolys;
        // bboxes.size() == expolys.size()
        BoundingBoxes bboxes;
        void compute_bb();
        ExPolygons intersections(const ExPolygons &to_clip) const;
        ExPolygons intersections(coord_t offset, const ExPolygons &to_clip) const;
        void clear();
        bool is_accept_all() const { return expolys.empty(); }
    };
    // encoded value of a setting or of a group of them.
    struct SettingsValue
    {
        std::vector<const ConfigOption*> key_options;
        std::vector<const ConfigOption *> val_options;
        SettingsValue() {};
        bool operator==(const SettingsValue &rhs) const;
        bool operator!=(const SettingsValue &rhs) const;
        bool operator<(const SettingsValue &rhs) const;
        static inline FloatOrPercent NONE{0, false};
        const ConfigOption *get_value_opt(const ConfigOption *opt = nullptr) const {
            assert(key_options.size() == val_options.size());
            if (opt == nullptr && val_options.size() >= 1) {
                return val_options.front();
            }
            for (size_t i = 0; i < key_options.size(); i++) {
                if (opt == key_options[i]) {
                    return val_options[i];
                }
            }
            assert(false);
            return opt;
        }
        bool is_percent(const ConfigOption *opt = nullptr) const { return get_value_opt(opt)->is_percent(); }
        double get_float(const ConfigOption *opt = nullptr) const { return get_value_opt(opt)->get_float(); }
        double get_effective_value(double ratio, const ConfigOption *opt = nullptr) const {
            return get_value_opt(opt)->get_effective_value(ratio);
        }
        int32_t get_int(const ConfigOption *opt = nullptr) const { return get_value_opt(opt)->get_int(); }
        bool get_bool(const ConfigOption *opt = nullptr) const { return get_value_opt(opt)->get_bool(); }
        bool is_enabled(const ConfigOption *opt = nullptr) const { return get_value_opt(opt)->is_enabled(); }
        bool empty() const { return val_options.empty(); }
        static inline SettingsValue create(std::vector<const ConfigOption *> default_options,
                                            std::vector<const ConfigOption *> options) {
            SettingsValue instance;
            instance.key_options = std::move(default_options);
            for (const ConfigOption *opt : options) {
                instance.val_options.push_back(opt);
            }
            return instance;
        }
        static inline SettingsValue create(const PrintRegionConfig &defaut_config,
                                            const PrintRegionConfig &config,
                                            t_config_option_keys opt_keys) {
            std::vector<const ConfigOption*> default_options;
            std::vector<const ConfigOption*> options;
            for (const t_config_option_key &opt_key : opt_keys) {
                default_options.push_back(defaut_config.option(opt_key));
                options.push_back(config.option(opt_key));
            }
            return create(default_options, options);
        }
    };

    RegionSettings(const PrintRegionConfig &default_config, const std::vector<t_config_option_keys> &available_keys)
        : config(default_config), opt_key_set(available_keys) {}

    bool has_many_config(const ConfigOption *opt) const {
        auto it = key_areas.find(opt);
        return it != key_areas.end() && it->second.size() > 1;
    }

    const std::map<SettingsValue, ClipExpoly> get_areas(const ConfigOption *opt) const {
        assert(key_areas.find(opt) != key_areas.end());
        return key_areas.at(opt);
    }

    const SettingsValue& get_solo_config(const ConfigOption *opt) const {
        auto it = key_areas.find(opt);
        assert(it != key_areas.end() && it->second.size() == 1);
        return it->second.begin()->first;
    }

    void segregate_regions(const ExPolygon &my_srf, const LayerRegionSetCPtrs &regions);

    std::map<SettingsValue, ClipExpoly>& create_extra_setting(const ConfigOption *opt) {
        return key_areas[opt];
    }

private:
    // region-specific parameters
    // ptr from this->config storage to ptr from a LayerRegion & combined areas for that value
    // if only one value in the inner map, then it means it's hte same value for evrything.
    std::map<const ConfigOption *, std::map<SettingsValue, ClipExpoly>> key_areas;

    // default config
    const PrintRegionConfig &config;
    const std::vector<t_config_option_keys> &opt_key_set;
};

} // namespace Slic3r

#endif
