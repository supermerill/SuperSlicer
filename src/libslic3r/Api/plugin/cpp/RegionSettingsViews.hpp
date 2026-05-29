///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_RegionSettingsViews_hpp_
#define slic3r_Api_plugin_cpp_RegionSettingsViews_hpp_

#include <cassert>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"

namespace slic3r_api {

/*
RegionSettings
==============

Helper for algorithms that process a whole LayerIsland but still need to know
whether a setting has one value everywhere or different values in different
regions/modifier areas.

Typical use:

    RegionSettings settings(storage, island, {{"extra_perimeters_odd_layers"}});
    settings.segregate(island.slice());

    if (!settings.has_many_config("extra_perimeters_odd_layers") &&
        settings.get_solo_config("extra_perimeters_odd_layers").get_bool()) {
        // Fast path: the whole area uses the same value.
    }

    for (const auto &[value, clip] : settings.get_areas("extra_perimeters_odd_layers")) {
        if (value.get_bool()) {
            StoredExPolygonCollection active =
                clip.intersections(my_candidate_expolygons);
            // Process only the part where this value applies.
        }
    }

An empty clip means "accept all". This matches the native RegionSettings
convention and avoids allocating geometry for the common single-value case.
*/

class RegionSettingsValue
{
public:
    RegionSettingsValue() = default;

    bool empty() const { return m_options.empty(); }
    size_t size() const { return m_options.size(); }

    bool operator==(const RegionSettingsValue &rhs) const {
        if (m_options.size() != rhs.m_options.size())
            return false;
        for (size_t idx = 0; idx < m_options.size(); ++idx) {
            if (m_options[idx] == nullptr || rhs.m_options[idx] == nullptr)
                return m_options[idx] == rhs.m_options[idx];
            if (config_option_equals(m_options[idx], rhs.m_options[idx]) == 0)
                return false;
        }
        return true;
    }

    bool operator!=(const RegionSettingsValue &rhs) const { return !(*this == rhs); }

    bool operator<(const RegionSettingsValue &rhs) const {
        if (m_options.size() != rhs.m_options.size())
            return m_options.size() < rhs.m_options.size();
        for (size_t idx = 0; idx < m_options.size(); ++idx) {
            const config_option_handle *lhs_opt = m_options[idx];
            const config_option_handle *rhs_opt = rhs.m_options[idx];
            if (lhs_opt == rhs_opt)
                continue;
            if (lhs_opt == nullptr || rhs_opt == nullptr)
                return lhs_opt < rhs_opt;
            if (config_option_equals(lhs_opt, rhs_opt) != 0)
                continue;
            return config_option_less(lhs_opt, rhs_opt) != 0;
        }
        return false;
    }

    ConfigOption option(const char *key = nullptr) const {
        const config_option_handle *opt = option_handle(key);
        assert(opt != nullptr);
        return ConfigOption(opt);
    }

    bool get_bool(const char *key = nullptr, uint32_t idx = 0) const {
        return option(key).get_bool(idx);
    }

    int32_t get_int(const char *key = nullptr, uint32_t idx = 0) const {
        return option(key).get_int(idx);
    }

    double get_float(const char *key = nullptr, uint32_t idx = 0) const {
        return option(key).get_float(idx);
    }

    bool is_percent(const char *key = nullptr, uint32_t idx = 0) const {
        return option(key).is_percent(idx);
    }

    double get_effective_value(double ratio, const char *key = nullptr, uint32_t idx = 0) const {
        return option(key).get_effective_value(ratio, idx);
    }

    bool is_enabled(const char *key = nullptr, uint32_t idx = 0) const {
        return option(key).is_enabled(idx);
    }

private:
    friend class RegionSettings;

    static RegionSettingsValue create(const Config &default_config,
                                      const Config &actual_config,
                                      const std::vector<std::string> &keys)
    {
        RegionSettingsValue out;
        out.m_keys = keys;
        out.m_options.reserve(keys.size());
        for (const std::string &key : keys) {
            const config_option_handle *actual = config_get(actual_config.handle(), key.c_str());
            const config_option_handle *fallback = config_get(default_config.handle(), key.c_str());
            out.m_options.push_back(actual != nullptr ? actual : fallback);
            assert(out.m_options.back() != nullptr);
        }
        return out;
    }

    const config_option_handle *option_handle(const char *key) const {
        if (key == nullptr) {
            assert(!m_options.empty());
            return m_options.empty() ? nullptr : m_options.front();
        }

        for (size_t idx = 0; idx < m_keys.size(); ++idx)
            if (std::strcmp(m_keys[idx].c_str(), key) == 0)
                return m_options[idx];

        assert(false);
        return nullptr;
    }

    std::vector<std::string> m_keys;
    std::vector<const config_option_handle *> m_options;
};

class RegionSettingsClip
{
public:
    explicit RegionSettingsClip(storage_handle *storage) : m_storage(storage) { assert(m_storage != nullptr); }
    RegionSettingsClip(RegionSettingsClip &&) noexcept = default;
    RegionSettingsClip &operator=(RegionSettingsClip &&) noexcept = default;
    RegionSettingsClip(const RegionSettingsClip &) = delete;
    RegionSettingsClip &operator=(const RegionSettingsClip &) = delete;

    // expolygons() be empty if only one region (it means there is no clip to do, evrything can be kept)
    bool is_accept_all() const { return m_expolygons == nullptr; }
    bool has_explicit_empty_geometry() const { return m_expolygons != nullptr && m_expolygons->empty(); }

    ExPolygonCollection expolygons() const {
        assert(m_expolygons != nullptr);
        return m_expolygons->readonly();
    }

    const expolygon_collection_handle *handle_or_null() const {
        return m_expolygons == nullptr ? nullptr : m_expolygons->handle();
    }

    StoredExPolygonCollection intersections(const ExPolygonCollection &subject) const {
        if (is_accept_all())
            return subject.clone(m_storage);
        ClipperContext clip(m_storage);
        return clipper_intersection(clip(subject), clip(m_expolygons->readonly())).to_expolygon_collection();
    }

    StoredExPolygonCollection intersections(coord_t offset, const ExPolygonCollection &subject) const {
        if (is_accept_all())
            return subject.clone(m_storage);
        ClipperContext clip(m_storage);
        ClipperOperand clip_area = offset == 0 ?
            clip(m_expolygons->readonly()) :
            clipper_offset(clip(m_expolygons->readonly()), double(offset));
        return clipper_intersection(clip(subject), clip_area).to_expolygon_collection();
    }

    StoredExPolygonCollection diff(const ExPolygonCollection &subject) const {
        if (is_accept_all())
            return StoredExPolygonCollection(m_storage);
        ClipperContext clip(m_storage);
        return clipper_diff(clip(subject), clip(m_expolygons->readonly())).to_expolygon_collection();
    }

    void append_copy_from(const ExPolygonCollection &expolygons) {
        ensure_collection().append_copy_from(expolygons);
    }

    void make_accept_all() { m_expolygons.reset(); }

    void union_self() {
        if (is_accept_all() || m_expolygons->empty())
            return;
        ClipperContext clip(m_storage);
        *m_expolygons = clipper_union(clip(m_expolygons->readonly())).to_expolygon_collection();
    }

private:
    friend class RegionSettings;

    StoredExPolygonCollection &ensure_collection() {
        if (m_expolygons == nullptr)
            m_expolygons = std::make_unique<StoredExPolygonCollection>(m_storage);
        return *m_expolygons;
    }
    
    // mutable method for RegionSettings
    void append_move_from(StoredExPolygonCollection &&expolygons) {
        ensure_collection().append_move_from(std::move(expolygons));
    }

    storage_handle *m_storage = nullptr;
    std::unique_ptr<StoredExPolygonCollection> m_expolygons;
};

class RegionSettings
{
public:
    using OptionKeyGroup = std::vector<std::string>;
    using AreaMap = std::map<RegionSettingsValue, RegionSettingsClip>;
    using RegionMap = std::map<RegionSettingsValue, std::vector<LayerRegion>>;

    RegionSettings(storage_handle *storage,
                   const Config &default_config,
                   std::vector<OptionKeyGroup> option_groups)
        : m_storage(storage), m_default_config(default_config), m_option_groups(std::move(option_groups))
    {
        assert(m_storage != nullptr);
    }

    RegionSettings(storage_handle *storage,
                   const Config &default_config,
                   std::initializer_list<std::initializer_list<const char *>> option_groups)
        : RegionSettings(storage, default_config, normalize_groups(option_groups))
    {}

    RegionSettings(storage_handle *storage,
                   const LayerIsland &island,
                   std::initializer_list<std::initializer_list<const char *>> option_groups)
        : RegionSettings(storage, first_region_config(island), normalize_groups(option_groups))
    {
        add_regions(island);
    }

    void clear_regions() { m_regions.clear(); }

    void add_region(const LayerRegion &region) {
        m_regions.push_back(region);
    }

    void add_regions(const LayerIsland &island) {
        for (uint32_t idx = 0; idx < island.region_count(); ++idx)
            add_region(island.region(idx));
    }

    void segregate(const ExPolygon &area) {
        m_key_areas.clear();
        m_key_regions.clear();
        ClipperContext clip(m_storage);

        for (const OptionKeyGroup &group : m_option_groups) {
            if (group.empty())
                continue;

            const std::string &primary_key = group.front();
            AreaMap areas;
            RegionMap regions;
            const RegionSettingsValue default_value =
                RegionSettingsValue::create(m_default_config, m_default_config, group);

            bool many_values = false;
            for (const LayerRegion &region : m_regions) {
                const RegionSettingsValue region_value =
                    RegionSettingsValue::create(m_default_config, region.print_region().config(), group);
                if (region_value != default_value) {
                    many_values = true;
                    break;
                }
            }

            if (many_values) {
                for (const LayerRegion &region : m_regions) {
                    const RegionSettingsValue region_value =
                        RegionSettingsValue::create(m_default_config, region.print_region().config(), group);
                    regions[region_value].push_back(region);
                    std::pair<AreaMap::iterator, bool> inserted =
                        areas.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(region_value),
                                      std::forward_as_tuple(m_storage));

                    if (!region.slices().empty()) {
                        StoredExPolygonCollection overlap =
                            clipper_intersection(clip(region.slices()), clip(area)).to_expolygon_collection();
                        if (!overlap.empty())
                            inserted.first->second.append_move_from(std::move(overlap));
                    }
                }

                for (AreaMap::iterator it = areas.begin(); it != areas.end();) {
                    if (it->second.has_explicit_empty_geometry())
                        it = areas.erase(it);
                    else
                        ++it;
                }

                if (areas.size() == 1)
                    areas.begin()->second.make_accept_all();
                else
                    for (std::pair<const RegionSettingsValue, RegionSettingsClip> &entry : areas)
                        entry.second.union_self();
            }

            if (areas.empty()) {
                areas.emplace(std::piecewise_construct,
                              std::forward_as_tuple(default_value),
                              std::forward_as_tuple(m_storage));
                regions[default_value] = m_regions;
            }

            m_key_areas.emplace(primary_key, std::move(areas));
            m_key_regions.emplace(primary_key, std::move(regions));
        }
    }

    bool has_many_config(const char *key) const {
        const AreaMap *areas = find_areas(key);
        return areas != nullptr && areas->size() > 1;
    }

    const AreaMap &get_areas(const char *key) const {
        const AreaMap *areas = find_areas(key);
        assert(areas != nullptr);
        return *areas;
    }

    const std::vector<LayerRegion> &get_regions(const char *key,
                                                const RegionSettingsValue &value) const
    {
        // get_areas() tells an algorithm where one value applies. get_regions()
        // answers the companion question: which source regions produced that
        // value. This is useful when a plugin must move clipped geometry into a
        // new LayerRegionIsland with the matching region ownership.
        const std::map<std::string, RegionMap>::const_iterator key_it = m_key_regions.find(key);
        assert(key_it != m_key_regions.end());
        const RegionMap::const_iterator value_it = key_it->second.find(value);
        assert(value_it != key_it->second.end());
        return value_it->second;
    }

    const RegionSettingsValue &get_solo_config(const char *key) const {
        const AreaMap &areas = get_areas(key);
        assert(areas.size() == 1);
        return areas.begin()->first;
    }

private:
    static Config first_region_config(const LayerIsland &island) {
        assert(island.region_count() > 0);
        return island.region(0).print_region().config();
    }

    static std::vector<OptionKeyGroup>
    normalize_groups(std::initializer_list<std::initializer_list<const char *>> option_groups)
    {
        std::vector<OptionKeyGroup> out;
        out.reserve(option_groups.size());
        for (std::initializer_list<const char *> group : option_groups) {
            OptionKeyGroup normalized;
            normalized.reserve(group.size());
            for (const char *key : group)
                if (key != nullptr)
                    normalized.emplace_back(key);
            out.push_back(std::move(normalized));
        }
        return out;
    }

    const AreaMap *find_areas(const char *key) const {
        assert(key != nullptr);
        const std::map<std::string, AreaMap>::const_iterator it = m_key_areas.find(key);
        return it == m_key_areas.end() ? nullptr : &it->second;
    }

    storage_handle *m_storage = nullptr;
    Config m_default_config;
    std::vector<OptionKeyGroup> m_option_groups;
    std::vector<LayerRegion> m_regions;
    std::map<std::string, AreaMap> m_key_areas;
    std::map<std::string, RegionMap> m_key_regions;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_RegionSettingsViews_hpp_
