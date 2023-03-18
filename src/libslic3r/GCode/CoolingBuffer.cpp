#include "../GCode.hpp"
#include "CoolingBuffer.hpp"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/log/trivial.hpp>
#include <iostream>
#include <float.h>
#include <unordered_set>

#if 0
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
#endif

#include <assert.h>

namespace Slic3r {

CoolingBuffer::CoolingBuffer(GCode &gcodegen) : m_config(gcodegen.config()), m_toolchange_prefix(gcodegen.writer().toolchange_prefix()), m_current_extruder(0)
{
    this->reset(gcodegen.writer().get_position());

    const std::vector<Extruder> &extruders = gcodegen.writer().extruders();
    m_extruder_ids.reserve(extruders.size());
    for (const Extruder &ex : extruders) {
        m_num_extruders = std::max(uint16_t(ex.id() + 1), m_num_extruders);
        m_extruder_ids.emplace_back(ex.id());
    }
}

void CoolingBuffer::reset(const Vec3d &position)
{
    m_current_pos.assign(5, 0.f);
    m_current_pos[0] = float(position.x());
    m_current_pos[1] = float(position.y());
    m_current_pos[2] = float(position.z());
    m_current_pos[4] = float(m_config.travel_speed.value);
    m_fan_speed = -1;
}

struct CoolingLine
{
    enum Type : uint64_t {
        //uint64_t
        //__int64
        //unsigned long long
        TYPE_SET_TOOL           = 1 << 0,
        TYPE_EXTRUDE_END        = 1 << 1,
        TYPE_BRIDGE_FAN_START   = 1 << 2,
        TYPE_BRIDGE_FAN_END     = 1 << 3,
        TYPE_BRIDGE_INTERNAL_FAN_START  = 1 << 4,
        TYPE_BRIDGE_INTERNAL_FAN_END    = 1 << 5,
        TYPE_TOP_FAN_START      = 1 << 6,
        TYPE_TOP_FAN_END        = 1 << 7,
        TYPE_SUPP_INTER_FAN_START = 1 << 8,
        TYPE_SUPP_INTER_FAN_END = 1 << 9,
        TYPE_G0                 = 1 << 10,
        TYPE_G1                 = 1 << 11,
        TYPE_ADJUSTABLE         = 1 << 12,
        TYPE_EXTERNAL_PERIMETER = 1 << 13,
        // The line sets a feedrate.
        TYPE_HAS_F              = 1 << 14,
        TYPE_WIPE               = 1 << 15,
        TYPE_G4                 = 1 << 16,
        TYPE_G92                = 1 << 17,
        //TYPE_STORE_FOR_WT       = 1 << 18,
        //TYPE_RESTORE_AFTER_WT   = 1 << 19,
        TYPE_SUPPORT_MAT_FAN_START     = 1 << 18,
        TYPE_SUPPORT_MAT_FAN_END       = 1 << 19,
        TYPE_INTERNAL_INFILL_FAN_START = 1 << 20,
        TYPE_INTERNAL_INFILL_FAN_END   = 1 << 21,
        TYPE_SOLID_INFILL_FAN_START        = 1 << 22,
        TYPE_SOLID_INFILL_FAN_END          = 1 << 23,
        TYPE_INTERNAL_PERI_FAN_START       = 1 << 24,
        TYPE_INTERNAL_PERI_FAN_END         = 1 << 25,
        TYPE_GAP_FILL_FAN_START            = 1 << 26,
        TYPE_GAP_FILL_FAN_END              = 1 << 27,
        TYPE_OVERHANG_PERI_FAN_START       = 1 << 28, //out of 32bit data range?
        TYPE_OVERHANG_PERI_FAN_END         = 1 << 29,
        TYPE_MIXED_FAN_START               = 1 << 30,
        TYPE_MIXED_FAN_END                 = 1 << 31,
        //TYPE_THIN_WALL_FAN_START         = 2 << 2,
        //TYPE_THIN_WALL_FAN_END           = 2 << 3,
        //TYPE_IRONING_FAN_START           = 2 << 4,
        //TYPE_IRONING_FAN_END             = 2 << 5,
        //TYPE_SKIRT_FAN_START             = 2 << 6,
        //TYPE_SKIRT_FAN_END               = 2 << 7,
        //TYPE_WIPE_TOWER_FAN_START        = 2 << 8,
        //TYPE_WIPE_TOWER_FAN_END          = 2 << 9,
        //TYPE_MILLING_FAN_START           = 2 << 10,
        //TYPE_MILLING_FAN_END             = 2 << 11,
        //TYPE_EXTERNAL_PERI_FAN_START       = 2 << 12,
        //TYPE_EXTERNAL_PERI_FAN_END         = 2 << 13,

        //missing types
        // erCustom
        // erMixed


        // Would be TYPE_ADJUSTABLE, but the block of G-code lines has zero extrusion length, thus the block
        // cannot have its speed adjusted. This should not happen (sic!).
        TYPE_ADJUSTABLE_EMPTY   = 1 << 12,
    };

    CoolingLine(unsigned int type, size_t  line_start, size_t  line_end) :
        type(type), line_start(line_start), line_end(line_end),
        length(0.f), feedrate(0.f), time(0.f), time_max(0.f), slowdown(false) {}

    bool adjustable(bool slowdown_external_perimeters) const {
        return (this->type & TYPE_ADJUSTABLE) && 
               (! (this->type & TYPE_EXTERNAL_PERIMETER) || slowdown_external_perimeters) &&
               this->time < this->time_max;
    }

    bool adjustable() const {
        return (this->type & TYPE_ADJUSTABLE) && this->time < this->time_max;
    }

    size_t  type;
    // Start of this line at the G-code snippet.
    size_t  line_start;
    // End of this line at the G-code snippet.
    size_t  line_end;
    // XY Euclidian length of this segment.
    float   length;
    // Current feedrate, possibly adjusted.
    float   feedrate;
    // Current duration of this segment.
    float   time;
    // Maximum duration of this segment.
    float   time_max;
    // If marked with the "slowdown" flag, the line has been slowed down.
    bool    slowdown;
    // for TYPE_SET_TOOL
    uint16_t new_tool;
};

// Calculate the required per extruder time stretches.
struct PerExtruderAdjustments 
{
    // Calculate the total elapsed time per this extruder, adjusted for the slowdown.
    float elapsed_time_total() const {
        float time_total = time_support;
        for (const CoolingLine &line : lines)
            time_total += line.time;
        return time_total;
    }
    // Calculate the total elapsed time when slowing down 
    // to the minimum extrusion feed rate defined for the current material.
    float maximum_time_after_slowdown(bool slowdown_external_perimeters) const {
        float time_total = time_support;
        for (const CoolingLine &line : lines)
            if (line.adjustable(slowdown_external_perimeters)) {
                if (line.time_max == FLT_MAX)
                    return FLT_MAX;
                else
                    time_total += line.time_max;
            } else
                time_total += line.time;
        return time_total;
    }
    // Calculate the adjustable part of the total time.
    float adjustable_time(bool slowdown_external_perimeters) const {
        float time_total = 0.f;
        for (const CoolingLine &line : lines)
            if (line.adjustable(slowdown_external_perimeters))
                time_total += line.time;
        return time_total;
    }
    // Calculate the non-adjustable part of the total time.
    float non_adjustable_time(bool slowdown_external_perimeters) const {
        float time_total = time_support;
        for (const CoolingLine &line : lines)
            if (! line.adjustable(slowdown_external_perimeters))
                time_total += line.time;
        return time_total;
    }
    // Slow down the adjustable extrusions to the minimum feedrate allowed for the current extruder material.
    // Used by both proportional and non-proportional slow down.
    float slowdown_to_minimum_feedrate(bool slowdown_external_perimeters) {
        float time_total = time_support;
        for (CoolingLine &line : lines) {
            if (line.adjustable(slowdown_external_perimeters)) {
                assert(line.time_max >= 0.f && line.time_max < FLT_MAX);
                line.slowdown = true;
                line.time     = line.time_max;
                assert(line.time > 0);
                line.feedrate = line.length / line.time;
            }
            time_total += line.time;
        }
        return time_total;
    }
    // Slow down each adjustable G-code line proportionally by a factor.
    // Used by the proportional slow down.
    float slow_down_proportional(float factor, bool slowdown_external_perimeters) {
        assert(factor >= 1.f);
        float time_total = time_support;
        for (CoolingLine &line : lines) {
            if (line.adjustable(slowdown_external_perimeters)) {
                line.slowdown = true;
                line.time     = std::min(line.time_max, line.time * factor);
                assert(line.time > 0);
                line.feedrate = line.length / line.time;
            }
            time_total += line.time;
        }
        return time_total;
    }

    // Sort the lines, adjustable first, higher feedrate first.
    // Used by non-proportional slow down.
    void sort_lines_by_decreasing_feedrate() {
        std::sort(lines.begin(), lines.end(), [](const CoolingLine &l1, const CoolingLine &l2) {
            bool adj1 = l1.adjustable();
            bool adj2 = l2.adjustable();
            return (adj1 == adj2) ? l1.feedrate > l2.feedrate : adj1;
        });
        for (n_lines_adjustable = 0; 
            n_lines_adjustable < lines.size() && this->lines[n_lines_adjustable].adjustable();
            ++ n_lines_adjustable);
        time_non_adjustable = 0.f;
        for (size_t i = n_lines_adjustable; i < lines.size(); ++ i)
            time_non_adjustable += lines[i].time;
    }

    // Calculate the maximum time stretch when slowing down to min_feedrate.
    // Slowdown to min_feedrate shall be allowed for this extruder's material.
    // Used by non-proportional slow down.
    float time_stretch_when_slowing_down_to_feedrate(float min_feedrate) const {
        float time_stretch = 0.f;
        assert(this->min_print_speed < min_feedrate + EPSILON);
        for (size_t i = 0; i < n_lines_adjustable; ++ i) {
            const CoolingLine &line = lines[i];
            if (line.feedrate > min_feedrate) {
                assert(min_feedrate > 0);
                time_stretch += line.time * (line.feedrate / min_feedrate - 1.f);
        }
        }
        return time_stretch;
    }

    // Slow down all adjustable lines down to min_feedrate.
    // Slowdown to min_feedrate shall be allowed for this extruder's material.
    // Used by non-proportional slow down.
    void slow_down_to_feedrate(float min_feedrate) {
        assert(this->min_print_speed < min_feedrate + EPSILON);
        for (size_t i = 0; i < n_lines_adjustable; ++ i) {
            CoolingLine &line = lines[i];
            if (line.feedrate > min_feedrate) {
                assert(min_feedrate > 0);
                line.time *= std::max(1.f, line.feedrate / min_feedrate);
                line.feedrate = min_feedrate;
                //test to never go over max_time
                if (line.time > line.time_max) {
                    line.time = line.time_max;
                    line.feedrate = line.length / line.time;
                }
                line.slowdown = true;
            }
        }
    }

    // Extruder, for which the G-code will be adjusted.
    unsigned int                extruder_id         = 0;
    // Is the cooling slow down logic enabled for this extruder's material?
    bool                        cooling_slow_down_enabled = false;
    // Slow down the print down to min_print_speed if the total layer time is below slowdown_below_layer_time.
    float                       slowdown_below_layer_time = 0.f;
    // Minimum print speed allowed for this extruder.
    float                       min_print_speed     = 0.f;
    // Max speed reduction allowed for this extruder.
    float                       max_speed_reduction = 1;

    // Parsed lines.
    std::vector<CoolingLine>    lines;
    // The following two values are set by sort_lines_by_decreasing_feedrate():
    // Number of adjustable lines, at the start of lines.
    size_t                      n_lines_adjustable  = 0;
    // Non-adjustable time of lines starting with n_lines_adjustable. 
    float                       time_non_adjustable = 0;
    // Current total time for this extruder.
    float                       time_total          = 0;
    // Maximum time for this extruder, when the maximum slow down is applied.
    float                       time_maximum = 0;
    //time spent on support from the previous layer
    float                       time_support = 0;

    // Temporaries for processing the slow down. Both thresholds go from 0 to n_lines_adjustable.
    size_t                      idx_line_begin      = 0;
    size_t                      idx_line_end        = 0;
};

// Calculate a new feedrate when slowing down by time_stretch for segments faster than min_feedrate.
// Used by non-proportional slow down.
float new_feedrate_to_reach_time_stretch(
    std::vector<PerExtruderAdjustments*>::const_iterator it_begin, std::vector<PerExtruderAdjustments*>::const_iterator it_end, 
    float min_feedrate, float time_stretch, size_t max_iter = 20)
{
	float new_feedrate = min_feedrate;
    for (size_t iter = 0; iter < max_iter; ++ iter) {
        float nomin = 0;
        float denom = time_stretch;
        for (auto it = it_begin; it != it_end; ++ it) {
			assert((*it)->min_print_speed < min_feedrate + EPSILON);
			for (size_t i = 0; i < (*it)->n_lines_adjustable; ++i) {
				const CoolingLine &line = (*it)->lines[i];
                if (line.feedrate > min_feedrate) {
                    nomin += line.time * line.feedrate;
                    denom += line.time;
                }
            }
        }
        assert(denom > 0);
        if (denom <= 0)
            return min_feedrate;
        new_feedrate = nomin / denom;
        assert(new_feedrate > min_feedrate - EPSILON);
        if (new_feedrate < min_feedrate + EPSILON)
            goto finished;
        for (auto it = it_begin; it != it_end; ++ it)
			for (size_t i = 0; i < (*it)->n_lines_adjustable; ++i) {
				const CoolingLine &line = (*it)->lines[i];
                if (line.feedrate > min_feedrate && line.feedrate < new_feedrate)
                    // Some of the line segments taken into account in the calculation of nomin / denom are now slower than new_feedrate, 
                    // which makes the new_feedrate lower than it should be.
                    // Re-run the calculation with a new min_feedrate limit, so that the segments with current feedrate lower than new_feedrate
                    // are not taken into account.
                    goto not_finished_yet;
            }
        goto finished;
not_finished_yet:
        min_feedrate = new_feedrate;
    }
    // Failed to find the new feedrate for the time_stretch.

finished:
    // Test whether the time_stretch was achieved.
#ifndef NDEBUG
    {
        float time_stretch_final = 0.f;
        for (auto it = it_begin; it != it_end; ++ it)
            time_stretch_final += (*it)->time_stretch_when_slowing_down_to_feedrate(new_feedrate);
        //assert(std::abs(time_stretch - time_stretch_final) < EPSILON);
    }
#endif /* NDEBUG */

	return new_feedrate;
}

std::string CoolingBuffer::process_layer(std::string &&gcode_in, size_t layer_id, bool flush, bool is_support_only)
{
    // Cache the input G-code.
    if (m_gcode.empty())
        m_gcode = std::move(gcode_in);
    else
        m_gcode += gcode_in;

    std::string out;
    if (flush) {
	    auto& previous_layer_time = is_support_only ? saved_layer_time_object : saved_layer_time_support;
	    auto my_previous_layer_time = is_support_only ? saved_layer_time_support : saved_layer_time_object;
	    auto& my_layer_time = is_support_only ? saved_layer_time_support : saved_layer_time_object;
	    std::vector<PerExtruderAdjustments> per_extruder_adjustments = this->parse_layer_gcode(m_gcode, m_current_pos);
	    //save our layer time in case of unchync
	    my_layer_time.clear();
	    for (PerExtruderAdjustments& adj : per_extruder_adjustments) {
	        my_layer_time[adj.extruder_id] = adj.elapsed_time_total();
	        auto it = my_previous_layer_time.find(adj.extruder_id);
	        if (it != my_previous_layer_time.end()) {
	            my_previous_layer_time[adj.extruder_id] = (my_previous_layer_time[adj.extruder_id] + my_layer_time[adj.extruder_id]) / 2 - my_layer_time[adj.extruder_id];
	        } else {
	            my_previous_layer_time[adj.extruder_id] = 0;
	        }
	    }
	    //add unsynch layer time
	    if (!previous_layer_time.empty()) {
	        for (PerExtruderAdjustments& adj : per_extruder_adjustments) {
	            auto it = previous_layer_time.find(adj.extruder_id);
	            if (it != previous_layer_time.end()) {
	                adj.time_support = it->second;
	            }
	        }
	        previous_layer_time.clear();
	    }
	    //add half diff with previous one, to avoid suddent change in fan speed.
	    if (!my_previous_layer_time.empty()) {
	        for (PerExtruderAdjustments& adj : per_extruder_adjustments) {
	            auto it = my_previous_layer_time.find(adj.extruder_id);
	            if (it != my_previous_layer_time.end()) {
	                adj.time_support += it->second;
	            }
	        }
	    }
	    //compute slowdown
	    float layer_time_stretched = this->calculate_layer_slowdown(per_extruder_adjustments);
	    //compute fans & gcode
	    out = this->apply_layer_cooldown(m_gcode, layer_id, layer_time_stretched, per_extruder_adjustments);
        m_gcode.clear();
    }
    return out;
}

// Parse the layer G-code for the moves, which could be adjusted.
// Return the list of parsed lines, bucketed by an extruder.
std::vector<PerExtruderAdjustments> CoolingBuffer::parse_layer_gcode(const std::string &gcode, std::vector<float> &current_pos) const
{
    std::vector<PerExtruderAdjustments> per_extruder_adjustments(m_extruder_ids.size());
    std::vector<size_t>                 map_extruder_to_per_extruder_adjustment(m_num_extruders, 0);
    for (size_t i = 0; i < m_extruder_ids.size(); ++ i) {
        PerExtruderAdjustments &adj         = per_extruder_adjustments[i];
        uint16_t                extruder_id = m_extruder_ids[i];
        adj.extruder_id               = extruder_id;
        adj.cooling_slow_down_enabled = m_config.slowdown_below_layer_time.get_at(extruder_id) > 0;
        adj.slowdown_below_layer_time = float(m_config.slowdown_below_layer_time.get_at(extruder_id));
        adj.min_print_speed           = float(m_config.min_print_speed.get_at(extruder_id));
        adj.max_speed_reduction       = float(m_config.max_speed_reduction.get_at(extruder_id) / 100);
        map_extruder_to_per_extruder_adjustment[extruder_id] = i;
    }

    uint16_t        current_extruder  = m_current_extruder;
    PerExtruderAdjustments *adjustment  = &per_extruder_adjustments[map_extruder_to_per_extruder_adjustment[current_extruder]];
    const char       *line_start = gcode.c_str();
    const char       *line_end   = line_start;
    const char        extrusion_axis = get_extrusion_axis(m_config)[0];
    // Index of an existing CoolingLine of the current adjustment, which holds the feedrate setting command
    // for a sequence of extrusion moves.
    size_t            active_speed_modifier = size_t(-1);

    for (; *line_start != 0; line_start = line_end) 
    {
        while (*line_end != '\n' && *line_end != 0)
            ++ line_end;
        // sline will not contain the trailing '\n'.
        std::string sline(line_start, line_end);
        // CoolingLine will contain the trailing '\n'.
        if (*line_end == '\n')
            ++ line_end;
        CoolingLine line(0, line_start - gcode.c_str(), line_end - gcode.c_str());
        if (boost::starts_with(sline, "G0 "))
            line.type = CoolingLine::TYPE_G0;
        else if (boost::starts_with(sline, "G1 "))
            line.type = CoolingLine::TYPE_G1;
        else if (boost::starts_with(sline, "G92 "))
            line.type = CoolingLine::TYPE_G92;
        if (line.type) {
            // G0, G1 or G92
            // Parse the G-code line.
            std::vector<float> new_pos(current_pos);
            const char *c = sline.data() + 3;
            for (;;) {
                // Skip whitespaces.
                for (; *c == ' ' || *c == '\t'; ++ c);
                if (*c == 0 || *c == ';')
                    break;

                assert(is_decimal_separator_point()); // for atof
                // Parse the axis.
                size_t axis = (*c >= 'X' && *c <= 'Z') ? (*c - 'X') :
                              (*c == extrusion_axis) ? 3 : (*c == 'F') ? 4 : size_t(-1);
                if (axis != size_t(-1)) {
                    new_pos[axis] = float(atof(++c));
                    if (axis == 4) {
                        // Convert mm/min to mm/sec.
                        new_pos[4] /= 60.f;
                        if ((line.type & CoolingLine::TYPE_G92) == 0)
                            // This is G0 or G1 line and it sets the feedrate. This mark is used for reducing the duplicate F calls.
                            line.type |= CoolingLine::TYPE_HAS_F;
                    }
                }
                // Skip this word.
                for (; *c != ' ' && *c != '\t' && *c != 0; ++ c);
            }
            bool external_perimeter = boost::contains(sline, ";_EXTERNAL_PERIMETER");
            bool wipe               = boost::contains(sline, ";_WIPE");
            if (external_perimeter)
                line.type |= CoolingLine::TYPE_EXTERNAL_PERIMETER;
            if (wipe)
                line.type |= CoolingLine::TYPE_WIPE;
            if (boost::contains(sline, ";_EXTRUDE_SET_SPEED") && ! wipe) {
                line.type |= CoolingLine::TYPE_ADJUSTABLE;
                active_speed_modifier = adjustment->lines.size();
            }
            if ((line.type & CoolingLine::TYPE_G92) == 0) {
                // G0 or G1. Calculate the duration.
                if (m_config.use_relative_e_distances.value)
                    // Reset extruder accumulator.
                    current_pos[3] = 0.f;
                float dif[4];
                for (size_t i = 0; i < 4; ++ i)
                    dif[i] = new_pos[i] - current_pos[i];
                float dxy2 = dif[0] * dif[0] + dif[1] * dif[1];
                float dxyz2 = dxy2 + dif[2] * dif[2];
                if (dxyz2 > 0.f) {
                    // Movement in xyz, calculate time from the xyz Euclidian distance.
                    line.length = sqrt(dxyz2);
                } else if (std::abs(dif[3]) > 0.f) {
                    // Movement in the extruder axis.
                    line.length = std::abs(dif[3]);
                }
                line.feedrate = new_pos[4];
                assert((line.type & CoolingLine::TYPE_ADJUSTABLE) == 0 || line.feedrate > 0.f);
                if (line.length > 0) {
                    assert(line.feedrate > 0);
                    line.time = line.length / line.feedrate;
                    assert(line.time > 0);
                }
                line.time_max = line.time;
                if ((line.type & CoolingLine::TYPE_ADJUSTABLE) || active_speed_modifier != size_t(-1)) {
                    assert(adjustment->min_print_speed >= 0);
                    line.time_max = (adjustment->min_print_speed == 0.f) ? FLT_MAX : std::max(line.time, line.length / adjustment->min_print_speed);
                    if(adjustment->max_speed_reduction > 0)
                        line.time_max = std::min(line.time_max, line.time / (1- adjustment->max_speed_reduction));
                }
                if (active_speed_modifier < adjustment->lines.size() && (line.type & CoolingLine::TYPE_G1)) {
                    // Inside the ";_EXTRUDE_SET_SPEED" blocks, there must not be a G1 Fxx entry.
                    assert((line.type & CoolingLine::TYPE_HAS_F) == 0);
                    CoolingLine &sm = adjustment->lines[active_speed_modifier];
                    assert(sm.feedrate > 0.f);
                    sm.length   += line.length;
                    sm.time     += line.time;
                    if (sm.time_max != FLT_MAX) {
                        if (line.time_max == FLT_MAX)
                            sm.time_max = FLT_MAX;
                        else
                            sm.time_max += line.time_max;
                    }
                    // Don't store this line.
                    line.type = 0;
                }
            }
            current_pos = std::move(new_pos);
        } else if (boost::starts_with(sline, ";_EXTRUDE_END")) {
            // Closing a block of non-zero length extrusion moves.
            line.type = CoolingLine::TYPE_EXTRUDE_END;
            if (active_speed_modifier != size_t(-1)) {
                assert(active_speed_modifier < adjustment->lines.size());
                CoolingLine &sm = adjustment->lines[active_speed_modifier];
                // There should be at least some extrusion move inside the adjustment block.
                // However if the block has no extrusion (which is wrong), fix it for the cooling buffer to work.
                assert(sm.length > 0);
                assert(sm.time > 0);
                if (sm.time <= 0) {
                    // Likely a zero length extrusion, it should not be emitted, however the zero extrusions should not confuse firmware either.
                    // Prohibit time adjustment of a block of zero length extrusions by the cooling buffer.
                    sm.type &= ~CoolingLine::TYPE_ADJUSTABLE;
                    // But the start / end comment shall be removed.
                    sm.type |= CoolingLine::TYPE_ADJUSTABLE_EMPTY;
                }
            }
            active_speed_modifier = size_t(-1);
        } else if (boost::starts_with(sline, m_toolchange_prefix) || boost::starts_with(sline, ";_TOOLCHANGE")) {
            int prefix = boost::starts_with(sline, ";_TOOLCHANGE") ? 13 : m_toolchange_prefix.size();
            uint16_t new_extruder = (uint16_t)atoi(sline.c_str() + prefix);
            // Only change extruder in case the number is meaningful. User could provide an out-of-range index through custom gcodes - those shall be ignored.
            if (new_extruder < map_extruder_to_per_extruder_adjustment.size()) {
                // Switch the tool.
                line.type = CoolingLine::TYPE_SET_TOOL;
                line.new_tool = new_extruder;
                if (new_extruder != current_extruder) {
                    current_extruder = new_extruder;
                    adjustment       = &per_extruder_adjustments[map_extruder_to_per_extruder_adjustment[current_extruder]];
                }
            }
            else {
                // Only log the error in case of MM printer. Single extruder printers likely ignore any T anyway.
                if (map_extruder_to_per_extruder_adjustment.size() > 1)
                    BOOST_LOG_TRIVIAL(error) << "CoolingBuffer encountered an invalid toolchange, maybe from a custom gcode: " << sline;
            }
        }
        else if (boost::starts_with(sline, ";_BRIDGE_FAN_START")) {
            line.type = CoolingLine::TYPE_BRIDGE_FAN_START;
        } else if (boost::starts_with(sline, ";_BRIDGE_FAN_END")) {
            line.type = CoolingLine::TYPE_BRIDGE_FAN_END;
        }
        else if (boost::starts_with(sline, ";_BRIDGE_INTERNAL_FAN_START")) {
            line.type = CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_START;
        } else if (boost::starts_with(sline, ";_BRIDGE_INTERNAL_FAN_END")) {
            line.type = CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_END;
        } 
        else if (boost::starts_with(sline, ";_TOP_FAN_START")) {
            line.type = CoolingLine::TYPE_TOP_FAN_START;
        } else if (boost::starts_with(sline, ";_TOP_FAN_END")) {
            line.type = CoolingLine::TYPE_TOP_FAN_END;

        } else if (boost::starts_with(sline, ";_SUPP_INTER_FAN_START")) {
            line.type = CoolingLine::TYPE_SUPP_INTER_FAN_START;
        } else if (boost::starts_with(sline, ";_SUPP_INTER_FAN_END")) {
            line.type = CoolingLine::TYPE_SUPP_INTER_FAN_END;

        } else if (boost::starts_with(sline, ";_SUPPORT_MAT_FAN_START")) {
            line.type = CoolingLine::TYPE_SUPPORT_MAT_FAN_START;
        } else if (boost::starts_with(sline, ";_SUPPORT_MAT_FAN_END")) {
            line.type = CoolingLine::TYPE_SUPPORT_MAT_FAN_END;

        } else if (boost::starts_with(sline, ";_INTERNAL_INFILL_FAN_START")) {
            line.type = CoolingLine::TYPE_INTERNAL_INFILL_FAN_START;
        } else if (boost::starts_with(sline, ";_INTERNAL_INFILL_FAN_END")) {
            line.type = CoolingLine::TYPE_INTERNAL_INFILL_FAN_END;

        } else if (boost::starts_with(sline, ";_SOLID_INFILL_FAN_START")) {
            line.type = CoolingLine::TYPE_SOLID_INFILL_FAN_START;
        } else if (boost::starts_with(sline, ";_SOLID_INFILL_FAN_END")) {
            line.type = CoolingLine::TYPE_SOLID_INFILL_FAN_END;

        } else if (boost::starts_with(sline, ";_INTERNAL_PERI_FAN_START")) {
            line.type = CoolingLine::TYPE_INTERNAL_PERI_FAN_START;
        } else if (boost::starts_with(sline, ";_INTERNAL_PERI_FAN_END")) {
            line.type = CoolingLine::TYPE_INTERNAL_PERI_FAN_END;

        } else if (boost::starts_with(sline, ";_OVERHANG_FAN_START")) {
            line.type = CoolingLine::TYPE_OVERHANG_PERI_FAN_START;
        } else if (boost::starts_with(sline, ";_OVERHANG_FAN_END")) {
            line.type = CoolingLine::TYPE_OVERHANG_PERI_FAN_END;

        } else if (boost::starts_with(sline, ";_GAP_FILL_FAN_START")) {
            line.type = CoolingLine::TYPE_GAP_FILL_FAN_START;
        } else if (boost::starts_with(sline, ";_GAP_FILL_FAN_END")) {
            line.type = CoolingLine::TYPE_GAP_FILL_FAN_END;

        } else if (boost::starts_with(sline, ";_THIN_WALL_FAN_START")) {             
            line.type = CoolingLine::TYPE_MIXED_FAN_START;
        } else if (boost::starts_with(sline, ";_THIN_WALL_FAN_END")) {
            line.type = CoolingLine::TYPE_MIXED_FAN_END;

        } else if (boost::starts_with(sline, ";_IRONING_FAN_START")) {          
            line.type = CoolingLine::TYPE_MIXED_FAN_START;
            //line.type = CoolingLine::TYPE_IRONING_FAN_START;
        } else if (boost::starts_with(sline, ";_IRONING_FAN_END")) {
            line.type = CoolingLine::TYPE_MIXED_FAN_END;
            //line.type = CoolingLine::TYPE_IRONING_FAN_END;

        } else if (boost::starts_with(sline, ";_WIPE_TOWER_FAN_START")) {
            line.type = CoolingLine::TYPE_MIXED_FAN_START;
            //line.type = CoolingLine::TYPE_WIPE_TOWER_FAN_START;
        } else if (boost::starts_with(sline, ";_WIPE_TOWER_FAN_END")) {
            line.type = CoolingLine::TYPE_MIXED_FAN_END;
            //line.type = CoolingLine::TYPE_WIPE_TOWER_FAN_END;

        } else if (boost::starts_with(sline, ";_MILLING_FAN_START")) {            
            //line.type = CoolingLine::TYPE_MILLING_FAN_START;
        } else if (boost::starts_with(sline, ";_MILLING_FAN_END")) {
            //line.type = CoolingLine::TYPE_MILLING_FAN_END;

        } else if (boost::starts_with(sline, ";_CUSTOM_FAN_START")) {
            //line.type = CoolingLine::TYPE_CUSTOM_FAN_START;
        } else if (boost::starts_with(sline, ";_CUSTOM_FAN_END")) {
            //line.type = CoolingLine::TYPE_CUSTOM_FAN_END;

        } else if (boost::starts_with(sline, ";_MIXED_FAN_START")) {
            //line.type = CoolingLine::TYPE_MIXED_FAN_START;
        } else if (boost::starts_with(sline, ";_MIXED_FAN_END")) {
            //line.type = CoolingLine::TYPE_MIXED_FAN_END;

        }/*else if (boost::starts_with(sline, ";_SKIRT_FAN_START")) {             
            line.type = CoolingLine::TYPE_SKIRT_FAN_START;
        } else if (boost::starts_with(sline, ";_SKIRT_FAN_END")) {
            line.type = CoolingLine::TYPE_SKIRT_FAN_END;
        }*/
        else if (boost::starts_with(sline, ";_EXTERNAL_PERI_FAN_START")) {
            //line.type = CoolingLine::TYPE_EXTERNAL_PERI_FAN_START; //last change comment this block out.
        } else if (boost::starts_with(sline, ";_EXTERNAL_PERI_FAN_END")) {
            //line.type = CoolingLine::TYPE_EXTERNAL_PERI_FAN_END;
        }


        else if (boost::starts_with(sline, "G4 ")) {
            // Parse the wait time.
            line.type = CoolingLine::TYPE_G4;
            size_t pos_S = sline.find('S', 3);
            size_t pos_P = sline.find('P', 3);
            assert(is_decimal_separator_point()); // for atof
            line.time = line.time_max = float(
                (pos_S > 0) ? atof(sline.c_str() + pos_S + 1) :
                (pos_P > 0) ? atof(sline.c_str() + pos_P + 1) * 0.001 : 0.);
        } else if (boost::starts_with(sline, ";_STORE_FAN_SPEED_WT")) {//not needed anymore
            //line.type = CoolingLine::TYPE_STORE_FOR_WT;
        } else if (boost::starts_with(sline, ";_RESTORE_FAN_SPEED_WT")) {//not needed anymore
            //line.type = CoolingLine::TYPE_RESTORE_AFTER_WT;
        }
        if (line.type != 0)
            adjustment->lines.emplace_back(std::move(line));
    }

    return per_extruder_adjustments;
}

// Slow down an extruder range proportionally down to slowdown_below_layer_time.
// Return the total time for the complete layer.
static inline float extruder_range_slow_down_proportional(
    std::vector<PerExtruderAdjustments*>::iterator it_begin,
    std::vector<PerExtruderAdjustments*>::iterator it_end,
    // Elapsed time for the extruders already processed.
    float elapsed_time_total0,
    // Initial total elapsed time before slow down.
    float elapsed_time_before_slowdown,
    // Target time for the complete layer (all extruders applied).
    float slowdown_below_layer_time)
{
    // Total layer time after the slow down has been applied.
    float total_after_slowdown = elapsed_time_before_slowdown;
    // Now decide, whether the external perimeters shall be slowed down as well.
    float max_time_nep = elapsed_time_total0;
    for (auto it = it_begin; it != it_end; ++ it)
        max_time_nep += (*it)->maximum_time_after_slowdown(false);
    if (max_time_nep > slowdown_below_layer_time) {
        // It is sufficient to slow down the non-external perimeter moves to reach the target layer time.
        // Slow down the non-external perimeters proportionally.
        float non_adjustable_time = elapsed_time_total0;
        for (auto it = it_begin; it != it_end; ++ it)
            non_adjustable_time += (*it)->non_adjustable_time(false);
        // The following step is a linear programming task due to the minimum movement speeds of the print moves.
        // Run maximum 5 iterations until a good enough approximation is reached.
        for (size_t iter = 0; iter < 5; ++ iter) {
            float factor = (slowdown_below_layer_time - non_adjustable_time) / (total_after_slowdown - non_adjustable_time);
            assert(factor > 1.f);
            total_after_slowdown = elapsed_time_total0;
            for (auto it = it_begin; it != it_end; ++ it)
                total_after_slowdown += (*it)->slow_down_proportional(factor, false);
            if (total_after_slowdown > 0.95f * slowdown_below_layer_time)
                break;
        }
    } else {
        // Slow down everything. First slow down the non-external perimeters to maximum.
        for (auto it = it_begin; it != it_end; ++ it)
            (*it)->slowdown_to_minimum_feedrate(false);
        // Slow down the external perimeters proportionally.
        float non_adjustable_time = elapsed_time_total0;
        for (auto it = it_begin; it != it_end; ++ it)
            non_adjustable_time += (*it)->non_adjustable_time(true);
        for (size_t iter = 0; iter < 5; ++ iter) {
            float factor = (slowdown_below_layer_time - non_adjustable_time) / (total_after_slowdown - non_adjustable_time);
            assert(factor > 1.f);
            total_after_slowdown = elapsed_time_total0;
            for (auto it = it_begin; it != it_end; ++ it)
                total_after_slowdown += (*it)->slow_down_proportional(factor, true);
            if (total_after_slowdown > 0.95f * slowdown_below_layer_time)
                break;
        }
    }
    return total_after_slowdown;
}

// Slow down an extruder range to slowdown_below_layer_time.
// Return the total time for the complete layer.
static inline void extruder_range_slow_down_non_proportional(
    std::vector<PerExtruderAdjustments*>::iterator it_begin,
    std::vector<PerExtruderAdjustments*>::iterator it_end,
    float time_stretch)
{
    // Slow down. Try to equalize the feedrates.
    std::vector<PerExtruderAdjustments*> by_min_print_speed(it_begin, it_end);
    // Find the next highest adjustable feedrate among the extruders.
    float feedrate = 0;
    for (PerExtruderAdjustments *adj : by_min_print_speed) {
        adj->idx_line_begin = 0;
        adj->idx_line_end   = 0;
        assert(adj->idx_line_begin < adj->n_lines_adjustable); //w8 for prusa to correct the root cause
        if (adj->lines[adj->idx_line_begin].feedrate > feedrate)
            feedrate = adj->lines[adj->idx_line_begin].feedrate;
    }
    assert(feedrate > 0.f);
    // Sort by min_print_speed, maximum speed first.
    // multiplied by max_speed_reduction to be able to sort them when only this one change.
    std::sort(by_min_print_speed.begin(), by_min_print_speed.end(), 
        [](const PerExtruderAdjustments *p1, const PerExtruderAdjustments *p2){ 
        return (1 - p1->max_speed_reduction) * p1->min_print_speed > (1 - p2->max_speed_reduction) * p2->min_print_speed; });
    // Slow down, fast moves first.
    for (;;) {
        // For each extruder, find the span of lines with a feedrate close to feedrate.
        for (PerExtruderAdjustments *adj : by_min_print_speed) {
            for (adj->idx_line_end = adj->idx_line_begin;
                adj->idx_line_end < adj->n_lines_adjustable && adj->lines[adj->idx_line_end].feedrate > feedrate - EPSILON;
                 ++ adj->idx_line_end) ;
        }
        // Find the next highest adjustable feedrate among the extruders.
        float feedrate_next = 0.f;
        for (PerExtruderAdjustments *adj : by_min_print_speed)
            if (adj->idx_line_end < adj->n_lines_adjustable && adj->lines[adj->idx_line_end].feedrate > feedrate_next)
                feedrate_next = adj->lines[adj->idx_line_end].feedrate;
        // Slow down, limited by max(feedrate_next, min_print_speed).
        for (auto adj = by_min_print_speed.begin(); adj != by_min_print_speed.end();) {
            // Slow down at most by time_stretch.
            //note: the max_speed reduction is used via the max_time, nothing else to do as it's a proportional limit.
            if ((*adj)->min_print_speed == 0.f) {
                // All the adjustable speeds are now lowered to the same speed,
                // and the minimum speed is set to zero.
                float time_adjustable = 0.f;
                for (auto it = adj; it != by_min_print_speed.end(); ++ it)
                    time_adjustable += (*it)->adjustable_time(true);
                assert(time_adjustable > 0);
                float rate = (time_adjustable + time_stretch) / time_adjustable;
                for (auto it = adj; it != by_min_print_speed.end(); ++ it)
                    (*it)->slow_down_proportional(rate, true);
                return;
            } else {
                float feedrate_limit = std::max(feedrate_next, (*adj)->min_print_speed);
                bool  done           = false;
                float time_stretch_max = 0.f;
                for (auto it = adj; it != by_min_print_speed.end(); ++ it)
                    time_stretch_max += (*it)->time_stretch_when_slowing_down_to_feedrate(feedrate_limit);
                if (time_stretch_max >= time_stretch) {
                    feedrate_limit = new_feedrate_to_reach_time_stretch(adj, by_min_print_speed.end(), feedrate_limit, time_stretch, 20);
                    done = true;
                } else
                    time_stretch -= time_stretch_max;
                for (auto it = adj; it != by_min_print_speed.end(); ++ it)
                    (*it)->slow_down_to_feedrate(feedrate_limit);
                if (done)
                    return;
            }
            // Skip the other extruders with nearly the same min_print_speed, as they have been processed already.
            auto next = adj;
            for (++ next; next != by_min_print_speed.end() && (*next)->min_print_speed > (*adj)->min_print_speed - EPSILON && (*next)->max_speed_reduction < (*adj)->max_speed_reduction + EPSILON; ++ next);
            adj = next;
        }
        if (feedrate_next == 0.f)
            // There are no other extrusions available for slow down.
            break;
        for (PerExtruderAdjustments *adj : by_min_print_speed) {
            adj->idx_line_begin = adj->idx_line_end;
            feedrate = feedrate_next;
        }
    }
}

// Calculate slow down for all the extruders.
float CoolingBuffer::calculate_layer_slowdown(std::vector<PerExtruderAdjustments> &per_extruder_adjustments)
{
    // Sort the extruders by an increasing slowdown_below_layer_time.
    // The layers with a lower slowdown_below_layer_time are slowed down
    // together with all the other layers with slowdown_below_layer_time above.
    std::vector<PerExtruderAdjustments*> by_slowdown_time;
    by_slowdown_time.reserve(per_extruder_adjustments.size());
    // Only insert entries, which are adjustable (have cooling enabled and non-zero stretchable time).
    // Collect total print time of non-adjustable extruders.
    float elapsed_time_total0 = 0.f;
    for (PerExtruderAdjustments &adj : per_extruder_adjustments) {
        // Curren total time for this extruder.
        adj.time_total  = adj.elapsed_time_total();
        // Maximum time for this extruder, when all extrusion moves are slowed down to min_extrusion_speed.
        adj.time_maximum = adj.maximum_time_after_slowdown(true);
        if (adj.cooling_slow_down_enabled && adj.lines.size() > 0) {
            by_slowdown_time.emplace_back(&adj);
            if (! m_cooling_logic_proportional)
                // sorts the lines, also sets adj.time_non_adjustable
                adj.sort_lines_by_decreasing_feedrate();
        } else
            elapsed_time_total0 += adj.elapsed_time_total();
    }
    std::sort(by_slowdown_time.begin(), by_slowdown_time.end(),
        [](const PerExtruderAdjustments *adj1, const PerExtruderAdjustments *adj2)
            { return adj1->slowdown_below_layer_time < adj2->slowdown_below_layer_time; });

    for (auto cur_begin = by_slowdown_time.begin(); cur_begin != by_slowdown_time.end(); ++ cur_begin) {
        PerExtruderAdjustments &adj = *(*cur_begin);
        // Calculate the current adjusted elapsed_time_total over the non-finalized extruders.
        float total = elapsed_time_total0;
        for (auto it = cur_begin; it != by_slowdown_time.end(); ++ it)
            total += (*it)->time_total;
        float slowdown_below_layer_time = adj.slowdown_below_layer_time * 1.001f;
        if (total > slowdown_below_layer_time) {
            // The current total time is above the minimum threshold of the rest of the extruders, don't adjust anything.
        } else {
            // Adjust this and all the following (higher m_config.slowdown_below_layer_time) extruders.
            // Sum maximum slow down time as if everything was slowed down including the external perimeters.
            float max_time = elapsed_time_total0;
            for (auto it = cur_begin; it != by_slowdown_time.end(); ++ it)
                max_time += (*it)->time_maximum;
            if (max_time > slowdown_below_layer_time) {
                if (m_cooling_logic_proportional)
                    extruder_range_slow_down_proportional(cur_begin, by_slowdown_time.end(), elapsed_time_total0, total, slowdown_below_layer_time);
                else
                    extruder_range_slow_down_non_proportional(cur_begin, by_slowdown_time.end(), slowdown_below_layer_time - total);
            } else {
                // Slow down to maximum possible.
                for (auto it = cur_begin; it != by_slowdown_time.end(); ++ it)
                    (*it)->slowdown_to_minimum_feedrate(true);
            }
        }
        elapsed_time_total0 += adj.elapsed_time_total();
    }

    return elapsed_time_total0;
}

// Apply slow down over G-code lines stored in per_extruder_adjustments, enable fan if needed.
// Returns the adjusted G-code.
std::string CoolingBuffer::apply_layer_cooldown(
    // Source G-code for the current layer.
    const std::string                      &gcode,
    // ID of the current layer, used to disable fan for the first n layers.
    size_t                                  layer_id, 
    // Total time of this layer after slow down, used to control the fan.
    float                                   layer_time,
    // Per extruder list of G-code lines and their cool down attributes.
    std::vector<PerExtruderAdjustments>    &per_extruder_adjustments)
{
    // First sort the adjustment lines by of multiple extruders by their position in the source G-code.
    std::vector<const CoolingLine*> lines;
    {
        size_t n_lines = 0;
        for (const PerExtruderAdjustments &adj : per_extruder_adjustments)
            n_lines += adj.lines.size();
        lines.reserve(n_lines);
        for (const PerExtruderAdjustments &adj : per_extruder_adjustments)
            for (const CoolingLine &line : adj.lines)
                lines.emplace_back(&line);
        std::sort(lines.begin(), lines.end(), [](const CoolingLine *ln1, const CoolingLine *ln2) { return ln1->line_start < ln2->line_start; } );
    }
    // Second generate the adjusted G-code.
    std::string new_gcode;
    new_gcode.reserve(gcode.size() * 2);
    bool bridge_fan_control = false;
    int  bridge_fan_speed = 0;
    bool bridge_internal_fan_control = false;
    int  bridge_internal_fan_speed = 0;
    bool top_fan_control = false;
    int  top_fan_speed = 0;
    //bool ext_peri_fan_control = false;
    bool min_fan_control                               = false;
    int  min_fan_speed                                 = 0;
    bool external_perimeter_fan_control                = false;
    int  external_perimeter_fan_speed                  = 0;
    bool ironing_fan_control                           = false;
    int  ironing_fan_speed                             = 0;
    bool internal_perimeter_fan_control                = false;
    int  internal_perimeter_fan_speed                  = 0;
    bool overhang_perimeter_fan_control                = false;
    int  overhang_perimeter_fan_speed                  = 0;
    bool thin_wall_fan_control                         = false;
    int  thin_wall_fan_speed                           = 0;
    bool gap_fill_fan_control                          = false;
    int  gap_fill_fan_speed                            = 0;
    bool support_material_interface_fan_control        = false;
    int  support_material_interface_fan_speed          = 0;
    bool support_material_fan_control                  = false; 
    int  support_material_fan_speed                    = 0;
    bool internal_infill_fan_control                   = false; 
    int  internal_infill_fan_speed                     = 0;
    bool solid_inifill_fan_control                     = false; 
    int  solid_infill_fan_speed                        = 0;
    bool skirt_fan_control                             = false; 
    int  skirt_fan_speed                               = 0;
    bool wipe_tower_fan_control                        = false; 
    int  wipe_tower_fan_speed                          = 0;
    //bool mixed_fan_control                             = false; 
    //int  mixed_fan_speed                               = 0;


#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_current_extruder)
    auto change_extruder_set_fan = [this, layer_id, layer_time, &new_gcode, 
        &bridge_fan_control, &bridge_fan_speed, &bridge_internal_fan_control, &bridge_internal_fan_speed, 
        &top_fan_control, &top_fan_speed,
        &external_perimeter_fan_control, &external_perimeter_fan_speed,
        &support_material_interface_fan_control, &support_material_interface_fan_speed,
        &support_material_fan_control, &support_material_fan_speed,
        &internal_infill_fan_control, &internal_infill_fan_speed,
        &solid_inifill_fan_control, &solid_infill_fan_speed,
        &internal_perimeter_fan_control, &internal_perimeter_fan_speed,
        &thin_wall_fan_control, &thin_wall_fan_speed,
        &overhang_perimeter_fan_control, &overhang_perimeter_fan_speed,
        &ironing_fan_control, &ironing_fan_speed,
        &gap_fill_fan_control, &gap_fill_fan_speed,
        &wipe_tower_fan_control, &wipe_tower_fan_speed
        //&mixed_fan_control, &mixed_fan_speed
        ](){

        int min_fan_speed = EXTRUDER_CONFIG(min_fan_speed);
        int disabled_fan_speed = 0;
        bridge_fan_speed = EXTRUDER_CONFIG(bridge_fan_speed);
        bridge_internal_fan_speed = EXTRUDER_CONFIG(bridge_internal_fan_speed);
        top_fan_speed = EXTRUDER_CONFIG(top_fan_speed);
        support_material_interface_fan_speed = EXTRUDER_CONFIG(support_material_interface_fan_speed);
        external_perimeter_fan_speed = EXTRUDER_CONFIG(external_perimeter_fan_speed);
        support_material_fan_speed = EXTRUDER_CONFIG(support_material_fan_speed);
        internal_infill_fan_speed = EXTRUDER_CONFIG(internal_infill_fan_speed);
        solid_infill_fan_speed = EXTRUDER_CONFIG(solid_infill_fan_speed);
        internal_perimeter_fan_speed = EXTRUDER_CONFIG(internal_perimeter_fan_speed);
        gap_fill_fan_speed = EXTRUDER_CONFIG(gap_fill_fan_speed);
        overhang_perimeter_fan_speed = EXTRUDER_CONFIG(overhang_perimeter_fan_speed);


        if (EXTRUDER_CONFIG(fan_always_on) == true){
            //ironing_fan_speed = EXTRUDER_CONFIG(ironing_fan_speed);
            ironing_fan_speed = min_fan_speed;
            //thin_wall_fan_speed = EXTRUDER_CONFIG(thin_wall_fan_speed);
            thin_wall_fan_speed = min_fan_speed;
            //wipe_tower_fan_speed = EXTRUDER_CONFIG(wipe_tower_fan_speed);
            wipe_tower_fan_speed = min_fan_speed;
            //mixed_fan_speed = EXTRUDER_CONFIG(min_fan_speed);
        }
        else if (EXTRUDER_CONFIG(fan_always_on) == false){
            ironing_fan_speed = top_fan_speed;
            thin_wall_fan_speed = external_perimeter_fan_speed;
            wipe_tower_fan_speed = external_perimeter_fan_speed;

        }
        // 0 is deprecated for disable: take care of temp settings.
        /*if (bridge_fan_speed == 0) bridge_fan_speed = -1;
        if (bridge_internal_fan_speed == 0) bridge_internal_fan_speed = -1;
        if (external_perimeter_fan_speed == 0) external_perimeter_fan_speed = -1;
        if (top_fan_speed == 0) top_fan_speed = -1;*/


// if any values are -1 for default give them the min_fan_speed
// if values are 1 disable fan set speed to 0

        if (bridge_fan_speed == 1) bridge_fan_speed = 0;
        if (bridge_internal_fan_speed == 1) bridge_internal_fan_speed = 0;
        if (external_perimeter_fan_speed == 1) external_perimeter_fan_speed = 0;
        if (top_fan_speed == 1) top_fan_speed = 0;
        if (support_material_interface_fan_speed == 1) support_material_interface_fan_speed = 0;
        if (support_material_fan_speed == 1) support_material_fan_speed = 0;
        if (internal_infill_fan_speed == 1) internal_infill_fan_speed = 0;
        if (solid_infill_fan_speed == 1) solid_infill_fan_speed = 0;
        if (internal_perimeter_fan_speed == 1) internal_perimeter_fan_speed = 0;
        if (ironing_fan_speed == 1) ironing_fan_speed = 0;
        if (overhang_perimeter_fan_speed == 1) overhang_perimeter_fan_speed = 0;
        if (thin_wall_fan_speed == 1) thin_wall_fan_speed = 0;
        if (gap_fill_fan_speed == 1) gap_fill_fan_speed = 0;
        if (wipe_tower_fan_speed == 1) wipe_tower_fan_speed = 0;

        if (bridge_fan_speed == -1) bridge_fan_speed = min_fan_speed;
        if (bridge_internal_fan_speed == -1) bridge_internal_fan_speed = bridge_fan_speed;//use bridge  if internal is -1
        if (external_perimeter_fan_speed == -1) external_perimeter_fan_speed = min_fan_speed;
        if (top_fan_speed == -1) top_fan_speed = min_fan_speed;
        if (support_material_fan_speed == -1) support_material_fan_speed = min_fan_speed;
        if (support_material_interface_fan_speed == -1) support_material_interface_fan_speed = support_material_fan_speed;//use support if interface is -1
        if (internal_infill_fan_speed == -1) internal_infill_fan_speed = min_fan_speed;
        if (solid_infill_fan_speed == -1) solid_infill_fan_speed = internal_infill_fan_speed; //use internal if solid is -1
        if (internal_perimeter_fan_speed == -1) internal_perimeter_fan_speed = external_perimeter_fan_speed; //use external if internal is -1
        if (ironing_fan_speed == -1) ironing_fan_speed = min_fan_speed;
        if (overhang_perimeter_fan_speed == -1) overhang_perimeter_fan_speed = min_fan_speed;
        if (thin_wall_fan_speed == -1) thin_wall_fan_speed = min_fan_speed;
        if (gap_fill_fan_speed == -1) gap_fill_fan_speed = min_fan_speed;
        if (wipe_tower_fan_speed == -1) wipe_tower_fan_speed = min_fan_speed;

        // end deprecation
        int fan_speed_new = EXTRUDER_CONFIG(fan_always_on) ? min_fan_speed : 0;
        int fan_speed_last = fan_speed_new;
        int disable_fan_first_layers = EXTRUDER_CONFIG(disable_fan_first_layers);
        bool apply_full_fan_at_layer = false;
        int full_fan_speed_layer = EXTRUDER_CONFIG(full_fan_speed_layer);

        if(full_fan_speed_layer < int(layer_id) )
        {
            apply_full_fan_at_layer = true;
        }
        else
            apply_full_fan_at_layer = false;
        if (full_fan_speed_layer == 0)
        {
            apply_full_fan_at_layer = false;
        }

        
        // Is the fan speed ramp enabled?

        //if (disable_fan_first_layers <= 0 && full_fan_speed_layer > 0) {
            // When ramping up fan speed from disable_fan_first_layers to full_fan_speed_layer, force disable_fan_first_layers above zero,
            // so there will be a zero fan speed at least at the 1st layer.
        //    disable_fan_first_layers = 1;
        //}
        if (int(layer_id) >= disable_fan_first_layers) {
            int   max_fan_speed             = EXTRUDER_CONFIG(max_fan_speed);
            float slowdown_below_layer_time = float(EXTRUDER_CONFIG(slowdown_below_layer_time));
            float fan_below_layer_time      = float(EXTRUDER_CONFIG(fan_below_layer_time));

            if (apply_full_fan_at_layer == false) {
                if (layer_time < slowdown_below_layer_time && fan_below_layer_time > 0) {
                    // Layer time very short. Enable the fan to a full throttle.
                    fan_speed_new = std::max(max_fan_speed, fan_speed_new);
                    bridge_fan_speed = std::max(max_fan_speed, bridge_fan_speed);
                    bridge_internal_fan_speed = std::max(max_fan_speed, bridge_internal_fan_speed);
                    external_perimeter_fan_speed = std::max(max_fan_speed, external_perimeter_fan_speed);
                    top_fan_speed = std::max(max_fan_speed, top_fan_speed);
                    support_material_fan_speed = std::max(max_fan_speed, support_material_fan_speed);
                    support_material_interface_fan_speed = std::max(max_fan_speed, support_material_interface_fan_speed);
                    internal_infill_fan_speed = std::max(max_fan_speed, internal_infill_fan_speed);
                    solid_infill_fan_speed = std::max(max_fan_speed, solid_infill_fan_speed);
                    internal_perimeter_fan_speed = std::max(max_fan_speed, internal_perimeter_fan_speed);
                    ironing_fan_speed = std::max(max_fan_speed, ironing_fan_speed);
                    overhang_perimeter_fan_speed = std::max(max_fan_speed, overhang_perimeter_fan_speed);
                    thin_wall_fan_speed = std::max(max_fan_speed, thin_wall_fan_speed);
                    gap_fill_fan_speed  = std::max(max_fan_speed, gap_fill_fan_speed);
                    wipe_tower_fan_speed = std::max(max_fan_speed, wipe_tower_fan_speed);
                    //mixed_fan_speed = std::max(max_fan_speed, mixed_fan_speed);

                } else if (layer_time < fan_below_layer_time) {
                    // Layer time quite short. Enable the fan proportionally according to the current layer time.
                    assert(layer_time >= slowdown_below_layer_time);
                    double t = (layer_time - slowdown_below_layer_time) / (fan_below_layer_time - slowdown_below_layer_time);
                    if (fan_speed_new < max_fan_speed)
                        fan_speed_new = int(floor(t * min_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (bridge_fan_speed >= 0 && bridge_fan_speed < max_fan_speed)
                        bridge_fan_speed = int(floor(t * bridge_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (bridge_internal_fan_speed >= 0 && bridge_internal_fan_speed < max_fan_speed)
                        bridge_internal_fan_speed = int(floor(t * bridge_internal_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (external_perimeter_fan_speed >= 0 && external_perimeter_fan_speed < max_fan_speed)
                        external_perimeter_fan_speed = int(floor(t * external_perimeter_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (support_material_interface_fan_speed >= 0 && support_material_interface_fan_speed < max_fan_speed) 
                        support_material_interface_fan_speed = int(floor(t * support_material_interface_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (support_material_fan_speed >= 0 && support_material_fan_speed < max_fan_speed)
                        support_material_fan_speed = int(floor(t * support_material_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (internal_infill_fan_speed >= 0 && internal_infill_fan_speed < max_fan_speed)
                        internal_infill_fan_speed = int(floor(t * internal_infill_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (solid_infill_fan_speed >= 0 && solid_infill_fan_speed < max_fan_speed)
                        solid_infill_fan_speed = int(floor(t * solid_infill_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (internal_perimeter_fan_speed >= 0 && internal_perimeter_fan_speed < max_fan_speed)
                        internal_perimeter_fan_speed = int(floor(t * internal_perimeter_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (ironing_fan_speed >= 0 && ironing_fan_speed < max_fan_speed)
                        ironing_fan_speed = int(floor(t * ironing_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (overhang_perimeter_fan_speed >= 0 && overhang_perimeter_fan_speed < max_fan_speed)
                        overhang_perimeter_fan_speed = int(floor(t * overhang_perimeter_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (thin_wall_fan_speed >= 0 && thin_wall_fan_speed < max_fan_speed)
                        thin_wall_fan_speed = int(floor(t * thin_wall_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (gap_fill_fan_speed >= 0 && gap_fill_fan_speed < max_fan_speed)
                        gap_fill_fan_speed = int(floor(t * gap_fill_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    if (wipe_tower_fan_speed >= 0 && wipe_tower_fan_speed < max_fan_speed)
                        wipe_tower_fan_speed = int(floor(t * wipe_tower_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                    //if (mixed_fan_speed >= 0 && mixed_fan_speed < max_fan_speed)
                    //    mixed_fan_speed = int(floor(t * mixed_fan_speed + (1. - t) * max_fan_speed) + 0.5);

                    if (top_fan_speed >= 0 && top_fan_speed < max_fan_speed) // needs to be set, otherwise remaining layers keep this fan speed? bug still active???
                        top_fan_speed = int(floor(t * top_fan_speed + (1. - t) * max_fan_speed) + 0.5);
                }
            }



            //if (int(layer_id) >= disable_fan_first_layers && int(layer_id) + 1 < full_fan_speed_layer) {
            if (apply_full_fan_at_layer == true){//could prob add in fan_speed_new and fan_speed last here. they would need to be global variables since due to the way files are sliced. small bug with bridge fan speeds applying half of new value.

                float factor = float(int(layer_id + 1) - disable_fan_first_layers) / float(full_fan_speed_layer - disable_fan_first_layers);
                if (full_fan_speed_layer >=50){
                    factor = factor * factor;
                }
                //math gets broken if full_fan_speed_layer is too high ie 50,100
                fan_speed_new = std::clamp(int(float(fan_speed_new ) * factor + 0.5f), 0, 255);
                if (bridge_fan_speed >= 0)
                    bridge_fan_speed = std::clamp(int(float(bridge_fan_speed) * factor + 0.5f), 0, 255);
                if (bridge_internal_fan_speed >= 0)
                    bridge_internal_fan_speed = std::clamp(int(float(bridge_internal_fan_speed) * factor + 0.5f), 0, 255);
                if (external_perimeter_fan_speed >= 0)
                    external_perimeter_fan_speed = std::clamp(int(float(external_perimeter_fan_speed) * factor + 0.5f), 0, 255);
                if (top_fan_speed >= 0)
                    top_fan_speed = std::clamp(int(float(top_fan_speed) * factor + 0.5f), 0, 255);
                if (support_material_fan_speed >= 0)
                    support_material_fan_speed = std::clamp(int(float(support_material_fan_speed) * factor + 0.5f), 0, 255);
                if (support_material_interface_fan_speed >= 0)
                    support_material_interface_fan_speed = std::clamp(int(float(support_material_interface_fan_speed) * factor + 0.5f), 0, 255);
                if (internal_infill_fan_speed >= 0)
                    internal_infill_fan_speed = std::clamp(int(float(internal_infill_fan_speed) * factor + 0.5f), 0, 255);
                if (solid_infill_fan_speed >= 0)
                    solid_infill_fan_speed = std::clamp(int(float(solid_infill_fan_speed) * factor + 0.5f), 0, 255);
                if (internal_perimeter_fan_speed >= 0)
                    internal_perimeter_fan_speed = std::clamp(int(float(internal_perimeter_fan_speed) * factor + 0.5f), 0, 255);
                if (ironing_fan_speed >= 0)
                    ironing_fan_speed = std::clamp(int(float(ironing_fan_speed) * factor + 0.5f), 0, 255);
                if (overhang_perimeter_fan_speed >= 0)
                    overhang_perimeter_fan_speed = std::clamp(int(float(overhang_perimeter_fan_speed) * factor + 0.5f), 0, 255);
                if (thin_wall_fan_speed >= 0)
                    thin_wall_fan_speed = std::clamp(int(float(thin_wall_fan_speed) * factor + 0.5f), 0, 255);
                if (gap_fill_fan_speed >= 0)
                    gap_fill_fan_speed = std::clamp(int(float(gap_fill_fan_speed) * factor + 0.5f), 0, 255);
                if (wipe_tower_fan_speed >= 0)
                    wipe_tower_fan_speed = std::clamp(int(float(wipe_tower_fan_speed) * factor + 0.5f), 0, 255);
            
            }



//if(fan_speed_new != fan_speed_last)
//        if(fan_speed_new == bridge_fan_speed)
//            {bridge_fan_control = true}


            /*
            bridge_fan_control = bridge_fan_speed != fan_speed_new && bridge_fan_speed >= 0;
            bridge_internal_fan_control = bridge_internal_fan_speed != fan_speed_new && bridge_internal_fan_speed >= 0;
            top_fan_control = top_fan_speed != fan_speed_new && top_fan_speed >= 0;
            support_material_interface_fan_control = support_material_interface_fan_speed != fan_speed_new && support_material_interface_fan_speed >= 0;
            external_perimeter_fan_control = external_perimeter_fan_speed != fan_speed_new && external_perimeter_fan_speed >= 0;
            support_material_fan_control = support_material_fan_speed != fan_speed_new && support_material_fan_speed >= 0;
            internal_infill_fan_control = internal_infill_fan_speed != fan_speed_new && internal_infill_fan_speed >= 0;
            solid_inifill_fan_control = solid_infill_fan_speed != fan_speed_new && solid_infill_fan_speed >= 0;
            internal_perimeter_fan_control = internal_perimeter_fan_speed != fan_speed_new && internal_perimeter_fan_speed >= 0;
            ironing_fan_control = ironing_fan_speed != fan_speed_new && ironing_fan_speed >= 0;
            overhang_perimeter_fan_control = overhang_perimeter_fan_speed != fan_speed_new && overhang_perimeter_fan_speed >= 0;
            thin_wall_fan_control = thin_wall_fan_speed != fan_speed_new && thin_wall_fan_speed >= 0;
            gap_fill_fan_control = gap_fill_fan_speed != fan_speed_new && gap_fill_fan_speed >= 0;
            wipe_tower_fan_control = wipe_tower_fan_speed != fan_speed_new && wipe_tower_fan_speed >= 0;
            //mixed_fan_control = mixed_fan_speed != fan_speed_new && mixed_fan_speed >= 0;

            */
            bridge_fan_control                      = true;
            bridge_internal_fan_control             = true;
            top_fan_control                         = true;
            support_material_interface_fan_control  = true;
            external_perimeter_fan_control          = true;
            support_material_fan_control            = true;
            internal_infill_fan_control             = true;
            solid_inifill_fan_control               = true;
            internal_perimeter_fan_control          = true;
            ironing_fan_control                     = true;
            overhang_perimeter_fan_control          = true;
            thin_wall_fan_control                   = true;
            gap_fill_fan_control                    = true;
            wipe_tower_fan_control                  = true;

        } else {
            bridge_fan_control                     = false;
            bridge_fan_speed                       = 0;
            bridge_internal_fan_control            = false;
            bridge_internal_fan_speed              = 0;
            top_fan_control                        = false;
            top_fan_speed                          = 0;
            support_material_interface_fan_control = false;
            support_material_interface_fan_speed   = 0;
            external_perimeter_fan_control         = false;
            external_perimeter_fan_speed           = 0;
            support_material_fan_control           = false;
            support_material_fan_speed             = 0;
            internal_infill_fan_control            = false;
            internal_infill_fan_speed              = 0;
            solid_inifill_fan_control              = false;
            solid_infill_fan_speed                 = 0;
            internal_perimeter_fan_control         = false;
            internal_perimeter_fan_speed           = 0;
            ironing_fan_control                    = false;
            ironing_fan_speed                      = 0;
            overhang_perimeter_fan_control         = false;
            overhang_perimeter_fan_speed           = 0;
            thin_wall_fan_control                  = false;
            thin_wall_fan_speed                    = 0;
            gap_fill_fan_control                   = false;
            gap_fill_fan_speed                     = 0;
            wipe_tower_fan_control                 = false;
            wipe_tower_fan_speed                   = 0;
            //mixed_fan_control                    = false;
            //mixed_fan_speed                      = 0;       
            fan_speed_new                          = 0;
        }
        /*if (fan_speed_new != m_fan_speed) {
            m_fan_speed = fan_speed_new;//writes default fan speed at start
            new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, m_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"inital_fan_speed;");
            //new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, m_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage);
        }*/
    };
    //set to know all fan modifiers that can be applied ( TYPE_BRIDGE_FAN_END, TYPE_TOP_FAN_START, TYPE_SUPP_INTER_FAN_START, TYPE_EXTERNAL_PERIMETER).
    std::unordered_set<CoolingLine::Type> current_fan_sections;
    const char         *pos               = gcode.c_str();
    int                 current_feedrate  = 0;
    int                 stored_fan_speed = m_fan_speed;
    change_extruder_set_fan();
    for (const CoolingLine *line : lines) {
        const char *line_start  = gcode.c_str() + line->line_start;
        const char *line_end    = gcode.c_str() + line->line_end;
        bool fan_need_set = false;
        if (line_start > pos)
            new_gcode.append(pos, line_start - pos);
        if (line->type & CoolingLine::TYPE_SET_TOOL) {
            if (line->new_tool != m_current_extruder) {
                m_current_extruder = line->new_tool;
                change_extruder_set_fan();
            }
            //write line if it's not a cooling marker comment
            if (!boost::starts_with(line_start, ";_")) {
                new_gcode.append(line_start, line_end - line_start);
            }
        } /*else if (line->type & CoolingLine::TYPE_STORE_FOR_WT) {
            //stored_fan_speed = m_fan_speed;
        } else if (line->type & CoolingLine::TYPE_RESTORE_AFTER_WT) {
            new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, stored_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage, "restored");
            //new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, stored_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage);
        
        }*/ else if (line->type & CoolingLine::TYPE_BRIDGE_FAN_START) {
            if (bridge_fan_control && current_fan_sections.find(CoolingLine::TYPE_BRIDGE_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_BRIDGE_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_BRIDGE_FAN_END) {
            if (bridge_fan_control || current_fan_sections.find(CoolingLine::TYPE_BRIDGE_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;//shouldn't this be false?? or do you want to set fans speed at end of extrusion role ?
                current_fan_sections.erase(CoolingLine::TYPE_BRIDGE_FAN_START);
            }

        } else if (line->type & CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_START) {
            if (bridge_internal_fan_control && current_fan_sections.find(CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_END) {
            if (bridge_internal_fan_control || current_fan_sections.find(CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_START);
            }

        } else if (line->type & CoolingLine::TYPE_TOP_FAN_START) {
            if (top_fan_control && current_fan_sections.find(CoolingLine::TYPE_TOP_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_TOP_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_TOP_FAN_END) {
            if (top_fan_control || current_fan_sections.find(CoolingLine::TYPE_TOP_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_TOP_FAN_START);
            }

        } else if (line->type & CoolingLine::TYPE_SUPP_INTER_FAN_START) {
            if (support_material_interface_fan_control && current_fan_sections.find(CoolingLine::TYPE_SUPP_INTER_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_SUPP_INTER_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_SUPP_INTER_FAN_END) {
            if (support_material_interface_fan_control || current_fan_sections.find(CoolingLine::TYPE_SUPP_INTER_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_SUPP_INTER_FAN_START);
            }

        } else if (line->type & CoolingLine::TYPE_SUPPORT_MAT_FAN_START) {
            if (support_material_fan_control && current_fan_sections.find(CoolingLine::TYPE_SUPPORT_MAT_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_SUPPORT_MAT_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_SUPPORT_MAT_FAN_END) {
            if (support_material_fan_control || current_fan_sections.find(CoolingLine::TYPE_SUPPORT_MAT_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_SUPPORT_MAT_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_INTERNAL_INFILL_FAN_START) {//-----------------------
            if (internal_infill_fan_control && current_fan_sections.find(CoolingLine::TYPE_INTERNAL_INFILL_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_INTERNAL_INFILL_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_INTERNAL_INFILL_FAN_END) {//----------------------
            if (internal_infill_fan_control || current_fan_sections.find(CoolingLine::TYPE_INTERNAL_INFILL_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_INTERNAL_INFILL_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_SOLID_INFILL_FAN_START) {
            if (solid_inifill_fan_control && current_fan_sections.find(CoolingLine::TYPE_SOLID_INFILL_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_SOLID_INFILL_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_SOLID_INFILL_FAN_END) {
            if (solid_inifill_fan_control || current_fan_sections.find(CoolingLine::TYPE_SOLID_INFILL_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_SOLID_INFILL_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_INTERNAL_PERI_FAN_START) {//-------------------------
            if (internal_perimeter_fan_control && current_fan_sections.find(CoolingLine::TYPE_INTERNAL_PERI_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_INTERNAL_PERI_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_INTERNAL_PERI_FAN_END) {//----------------------------
            if (internal_perimeter_fan_control || current_fan_sections.find(CoolingLine::TYPE_INTERNAL_PERI_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_INTERNAL_PERI_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_GAP_FILL_FAN_START) {
            if (gap_fill_fan_control && current_fan_sections.find(CoolingLine::TYPE_GAP_FILL_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_GAP_FILL_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_GAP_FILL_FAN_END) {
            if (gap_fill_fan_control || current_fan_sections.find(CoolingLine::TYPE_GAP_FILL_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_GAP_FILL_FAN_START);
            }

        } else if (line->type & CoolingLine::TYPE_OVERHANG_PERI_FAN_START) {//-------------------------
            if (overhang_perimeter_fan_control && current_fan_sections.find(CoolingLine::TYPE_OVERHANG_PERI_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_OVERHANG_PERI_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_OVERHANG_PERI_FAN_END) {//-------------------------
            if (overhang_perimeter_fan_control || current_fan_sections.find(CoolingLine::TYPE_OVERHANG_PERI_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_OVERHANG_PERI_FAN_START);
            }

        }






        else if (line->type & CoolingLine::TYPE_MIXED_FAN_START) {
         //else if (line->type & CoolingLine::TYPE_THIN_WALL_FAN_START) {}
            if (thin_wall_fan_control && current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) == current_fan_sections.end()) {
            //if (thin_wall_fan_control && current_fan_sections.find(CoolingLine::TYPE_THIN_WALL_FAN_START) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_MIXED_FAN_START);
                //current_fan_sections.insert(CoolingLine::TYPE_THIN_WALL_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_MIXED_FAN_END) {
          //else if (line->type & CoolingLine::TYPE_THIN_WALL_FAN_END) {
            if (thin_wall_fan_control || current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) != current_fan_sections.end()) {
            //if (thin_wall_fan_control || current_fan_sections.find(CoolingLine::TYPE_THIN_WALL_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_MIXED_FAN_START);
                //thin_wall_fan_control = false;
                //current_fan_sections.erase(CoolingLine::TYPE_THIN_WALL_FAN_START);
            }

        } else if (line->type & CoolingLine::TYPE_MIXED_FAN_START) {
            //else if (line->type & CoolingLine::TYPE_IRONING_FAN_START) {
            if (ironing_fan_control && current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) == current_fan_sections.end()) {
            //if (ironing_fan_control || current_fan_sections.find(CoolingLine::TYPE_IRONING_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_MIXED_FAN_START);
                //current_fan_sections.insert(CoolingLine::TYPE_IRONING_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_MIXED_FAN_END) {
            //else if (line->type & CoolingLine::TYPE_IRONING_FAN_END) {
            if (ironing_fan_control || current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) != current_fan_sections.end()) {
            //if (ironing_fan_control || current_fan_sections.find(CoolingLine::TYPE_IRONING_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_MIXED_FAN_START);
                //ironing_fan_control = false;
                //current_fan_sections.erase(CoolingLine::TYPE_IRONING_FAN_START);
            }
        }else if (line->type & CoolingLine::TYPE_MIXED_FAN_START) {
            //else if (line->type & CoolingLine::TYPE_WIPE_TOWER_FAN_START) {
            if (wipe_tower_fan_control && current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) == current_fan_sections.end()) {
           // if (wipe_tower_fan_control || current_fan_sections.find(CoolingLine::TYPE_WIPE_TOWER_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_MIXED_FAN_START);
                //current_fan_sections.insert(CoolingLine::TYPE_WIPE_TOWER_FAN_START);
            }
        } else if (line->type & CoolingLine::TYPE_MIXED_FAN_END) {
            //else if (line->type & CoolingLine::TYPE_WIPE_TOWER_FAN_END) {
            if (wipe_tower_fan_control || current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) != current_fan_sections.end()) {
            //if (wipe_tower_fan_control || current_fan_sections.find(CoolingLine::TYPE_WIPE_TOWER_FAN_START) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_MIXED_FAN_START);
                //current_fan_sections.erase(CoolingLine::TYPE_WIPE_TOWER_FAN_START);
                //wipe_tower_fan_control = false;
            }
        }
        

        else if (line->type & CoolingLine::TYPE_EXTRUDE_END) {
            if (external_perimeter_fan_control || current_fan_sections.find(CoolingLine::TYPE_EXTERNAL_PERIMETER) != current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.erase(CoolingLine::TYPE_EXTERNAL_PERIMETER);
            }
        } else if (line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_ADJUSTABLE_EMPTY | CoolingLine::TYPE_EXTERNAL_PERIMETER | CoolingLine::TYPE_WIPE | CoolingLine::TYPE_HAS_F)) {
            //ext_peri_fan_speed
            if ((line->type & CoolingLine::TYPE_EXTERNAL_PERIMETER) != 0 && external_perimeter_fan_control && current_fan_sections.find(CoolingLine::TYPE_EXTERNAL_PERIMETER) == current_fan_sections.end()) {
                fan_need_set = true;
                current_fan_sections.insert(CoolingLine::TYPE_EXTERNAL_PERIMETER);
            }

            // Find the start of a comment, or roll to the end of line.
            const char *end = line_start;
            for (; end < line_end && *end != ';'; ++ end);
            // Find the 'F' word.
            const char *fpos            = strstr(line_start + 2, " F") + 2;
            int         new_feedrate    = current_feedrate;
            // Modify the F word of the current G-code line.
            bool        modify          = false;
            // Remove the F word from the current G-code line.
            bool        remove          = false;
            assert(fpos != nullptr);
            new_feedrate = line->slowdown ? int(floor(60. * line->feedrate + 0.5)) : atoi(fpos);
            if (new_feedrate == current_feedrate) {
                // No need to change the F value.
                if ((line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_ADJUSTABLE_EMPTY | CoolingLine::TYPE_EXTERNAL_PERIMETER | CoolingLine::TYPE_WIPE)) || line->length == 0.)
                    // Feedrate does not change and this line does not move the print head. Skip the complete G-code line including the G-code comment.
                    end = line_end;
                else
                    // Remove the feedrate from the G0/G1 line. The G-code line may become empty!
                    remove = true;
            } else if (line->slowdown) {
                // The F value will be overwritten.
                modify = true;
            } else {
                // The F value is different from current_feedrate, but not slowed down, thus the G-code line will not be modified.
                // Emit the line without the comment.
                new_gcode.append(line_start, end - line_start);
                current_feedrate = new_feedrate;
            }
            if (modify || remove) {
                if (modify) {
                    // Replace the feedrate.
                    new_gcode.append(line_start, fpos - line_start);
                    current_feedrate = new_feedrate;
                    char buf[64];
                    sprintf(buf, "%d", int(current_feedrate));
                    new_gcode += buf;
                } else {
                    // Remove the feedrate word.
                    const char *f = fpos;
                    // Roll the pointer before the 'F' word.
                    for (f -= 2; f > line_start && (*f == ' ' || *f == '\t'); -- f);
                    // Append up to the F word, without the trailing whitespace.
                    //but only if there are something else than a simple "G1" (F is always put at the end of a G1 command)
                    if(f - line_start > 2)
                        new_gcode.append(line_start, f - line_start + 1);
                }
                // Skip the non-whitespaces of the F parameter up the comment or end of line.
                for (; fpos != end && *fpos != ' ' && *fpos != ';' && *fpos != '\n'; ++ fpos);
                // Append the rest of the line without the comment.
                if (remove && (fpos == end || *fpos == '\n') && (new_gcode == "G1" || boost::ends_with(new_gcode, "\nG1"))) {
                    // The G-code line only contained the F word, now it is empty. Remove it completely including the comments.
                    new_gcode.resize(new_gcode.size() - 2);
                    end = line_end;
                } else {
                    // The G-code line may not be empty yet. Emit the rest of it.
                    new_gcode.append(fpos, end - fpos);
                }
            }
            // Process the rest of the line.
            if (end < line_end) {
                if (line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_ADJUSTABLE_EMPTY | CoolingLine::TYPE_EXTERNAL_PERIMETER | CoolingLine::TYPE_WIPE)) {
                    // Process comments, remove ";_EXTRUDE_SET_SPEED", ";_EXTERNAL_PERIMETER", ";_WIPE"
                    std::string comment(end, line_end);
                    boost::replace_all(comment, ";_EXTRUDE_SET_SPEED", "");
                    if (line->type & CoolingLine::TYPE_EXTERNAL_PERIMETER)
                        boost::replace_all(comment, ";_EXTERNAL_PERIMETER", "");
                    if (line->type & CoolingLine::TYPE_WIPE)
                        boost::replace_all(comment, ";_WIPE", "");
                    new_gcode += comment;
                } else {
                    // Just attach the rest of the source line.
                    new_gcode.append(end, line_end - end);
                }
            }
        } else {
            new_gcode.append(line_start, line_end - line_start);
        }
        if (fan_need_set == true) {
            //choose the speed with highest priority //new if statement doesn't matter for priority now.

            if (bridge_internal_fan_control && current_fan_sections.find(CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_BRIDGE_INTERNAL_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, bridge_internal_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"bridge_internl_fan_speed;");
            
            if (support_material_fan_control && current_fan_sections.find(CoolingLine::TYPE_SUPPORT_MAT_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_SUPPORT_MAT_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, support_material_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"support_material_fan_;;;;");
            
            if (internal_perimeter_fan_control && current_fan_sections.find(CoolingLine::TYPE_INTERNAL_PERI_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_INTERNAL_PERI_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, internal_perimeter_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"internal_perimeter_fan_;;");

            if (overhang_perimeter_fan_control && current_fan_sections.find(CoolingLine::TYPE_OVERHANG_PERI_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_OVERHANG_PERI_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, overhang_perimeter_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"overhang_perimeter_fan_;;");

            if (solid_inifill_fan_control && current_fan_sections.find(CoolingLine::TYPE_SOLID_INFILL_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_SOLID_INFILL_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, solid_infill_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"solid_infill_fan_speed;;;");
                    
            if (internal_infill_fan_control && current_fan_sections.find(CoolingLine::TYPE_INTERNAL_INFILL_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_INTERNAL_INFILL_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, internal_infill_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"internal_infil_fan_speed;");                
            
            if (support_material_interface_fan_control && current_fan_sections.find(CoolingLine::TYPE_SUPP_INTER_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_SUPP_INTER_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, support_material_interface_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"support_interface_fan;;;;");
                
            if (top_fan_control && current_fan_sections.find(CoolingLine::TYPE_TOP_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_TOP_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, top_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"top_fan_speed_;;;;;;;;;;;;");

            if (external_perimeter_fan_control && current_fan_sections.find(CoolingLine::TYPE_EXTERNAL_PERIMETER) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_EXTERNAL_PERIMETER)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, external_perimeter_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"ext_peri_fan_speed;;;;;;;");

            if (gap_fill_fan_control && current_fan_sections.find(CoolingLine::TYPE_GAP_FILL_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_GAP_FILL_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, gap_fill_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"gap_fill_fan_;;;;;;;;;;;;");
                
            if (bridge_fan_control && current_fan_sections.find(CoolingLine::TYPE_BRIDGE_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_BRIDGE_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, bridge_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"bridge_fan_speed;;;;;;;;;");

            //else if needed to prevent double ups of fan types getting set.priority
            if (thin_wall_fan_control && current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_MIXED_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, thin_wall_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"Mixed_fan_speed;;;;;;;;;;");
          
            else if (ironing_fan_control && current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_MIXED_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, ironing_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"Mixed_fan_speed;;;;;;;;;;");

            else if (wipe_tower_fan_control && current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_MIXED_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, wipe_tower_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"Mixed_fan_speed;;;;;;;;;;");



            /*
            if (ironing_fan_control && current_fan_sections.find(CoolingLine::TYPE_IRONING_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_IRONING_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, ironing_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"ironing_fan_speed_;;;;;;;;;;;;");
                    //new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, ironing_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage);

            if (thin_wall_fan_control && current_fan_sections.find(CoolingLine::TYPE_THIN_WALL_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_THIN_WALL_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, thin_wall_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"thin_wall_fan_speed_;;;;;;;;;;;;");
                    //new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, thin_wall_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage);

            if (wipe_tower_fan_control && current_fan_sections.find(CoolingLine::TYPE_WIPE_TOWER_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_WIPE_TOWER_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, wipe_tower_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"wipe_tower_fan_speed_;;;;;;;;;;;;");
                    //new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, wipe_tower_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage);

            */

            /*if (mixed_fan_control && current_fan_sections.find(CoolingLine::TYPE_MIXED_FAN_START) != current_fan_sections.end())
                if (line->type & CoolingLine::TYPE_MIXED_FAN_START)
                    new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, mixed_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"mixed_fan_speed_;;;;;;;;;;;;");
                    //new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, mixed_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage);
            */
            else
                new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, min_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,"restored fan toolchange;;");
                //new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, m_fan_speed, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage);
                //new_gcode  += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, -1, EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,";error finding cooling line");
                fan_need_set = false;
        }
        pos = line_end;
    }
    #undef EXTRUDER_CONFIG
        const char *gcode_end = gcode.c_str() + gcode.size();
        if (pos < gcode_end)
            new_gcode.append(pos, gcode_end - pos);

        // There should be no empty G1 lines emitted.
        assert(new_gcode.find("G1\n") == std::string::npos);
        return new_gcode;
    }

} // namespace Slic3r
