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

CoolingBuffer::CoolingBuffer(GCode &gcodegen) : m_config(gcodegen.config()), m_current_extruder(0)
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
    m_current_pos.assign(7, 0.f);
    m_current_pos[0] = float(position.x());
    m_current_pos[1] = float(position.y());
    m_current_pos[2] = float(position.z());
    // 3 : e
    m_current_pos[4] = float(m_config.travel_speed.value);
    // 5 : i (G2/G3)
    // 6 : j (G2/G3)
    m_fan_speed = -1;
}

struct CoolingLine
{
    enum Type : uint32_t {
        TYPE_NONE               = 0,
        //first 5 bits are for the extrusiontype (not a flag)

        TYPE_SET_TOOL           = 1 << 7,
        TYPE_EXTRUDE_START      = 1 << 8,
        TYPE_EXTRUDE_END        = 1 << 9,
        TYPE_G0                 = 1 << 10,
        TYPE_G1                 = 1 << 11,
        // if adjustable, that means that the lines after that can't have their speed modified. end after a TYPE_EXTRUDE_END
        TYPE_ADJUSTABLE         = 1 << 12,
        // TYPE_ADJUSTABLE_MAYBE = do not adjust this section if possible
        TYPE_ADJUSTABLE_MAYBE   = 1 << 13,
        // The line sets a feedrate.
        TYPE_HAS_F              = 1 << 14,
        TYPE_WIPE               = 1 << 15,
        TYPE_G4                 = 1 << 16,
        TYPE_G92                = 1 << 17,
        TYPE_STORE_FOR_WT       = 1 << 18,
        TYPE_RESTORE_AFTER_WT   = 1 << 19,
        //BBS: add G2 G3 type
        TYPE_G2                 = 1 << 20,
        TYPE_G3                 = 1 << 21,
        // Would be TYPE_ADJUSTABLE, but the block of G-code lines has zero extrusion length, thus the block
        // cannot have its speed adjusted. This should not happen (sic!). (delete the block if no g1)
        TYPE_ADJUSTABLE_EMPTY   = 1 << 22,
    };
    static inline ExtrusionRole to_extrusion_role(uint32_t type) {
        return ExtrusionRole(uint8_t(type & 0x1F));
    }

    CoolingLine(unsigned int type, size_t  line_start, size_t  line_end) :
        type(type), line_start(line_start), line_end(line_end),
        length(0.f), has_move(false), feedrate(0.f), time(0.f), time_max(0.f), slowdown(false) {}

    bool adjustable(bool slowdown_external_perimeters) const {
        return (this->type & TYPE_ADJUSTABLE) && 
               (slowdown_external_perimeters || (!(this->type & TYPE_ADJUSTABLE_MAYBE))) &&
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
    // is this line move at least a coordinate (x, y, z, e, i,j) (iirc, it's everything but F)
    bool    has_move;
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
        std::map<size_t, float>& last_different_layer_time = is_support_only ? saved_layer_time_object : saved_layer_time_support;
        // previous_layer_time is also use at temporary store for the diff between our layer_time and its.
        std::map<size_t, float>  previous_layer_time = is_support_only ? saved_layer_time_support : saved_layer_time_object;
        std::map<size_t, float>& my_layer_time = previous_layer_time;
        std::vector<PerExtruderAdjustments> per_extruder_adjustments = this->parse_layer_gcode(m_gcode, m_current_pos);
        //save our layer time in case of unchync
        my_layer_time.clear();
        for (PerExtruderAdjustments& adj : per_extruder_adjustments) {
            my_layer_time[adj.extruder_id] = adj.elapsed_time_total();
            auto it = previous_layer_time.find(adj.extruder_id);
            if (it != previous_layer_time.end()) {
                previous_layer_time[adj.extruder_id] = (previous_layer_time[adj.extruder_id] + my_layer_time[adj.extruder_id]) / 2 - my_layer_time[adj.extruder_id];
            } else {
                previous_layer_time[adj.extruder_id] = 0;
            }
        }
        //add unsynch layer time (support for object, and object for support)
        if (!last_different_layer_time.empty()) {
            for (PerExtruderAdjustments& adj : per_extruder_adjustments) {
                auto it = last_different_layer_time.find(adj.extruder_id);
                if (it != last_different_layer_time.end()) {
                    adj.time_support += it->second;
                }
            }
            last_different_layer_time.clear();
        }
        //add half diff with previous one, to avoid flip-flop quick change in fan speed.
        if (!previous_layer_time.empty()) {
            for (PerExtruderAdjustments& adj : per_extruder_adjustments) {
                auto it = previous_layer_time.find(adj.extruder_id);
                if (it != previous_layer_time.end()) {
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
    // type to add to each next G1 (just for adjustable for now)
    size_t            current_stamp = CoolingLine::TYPE_NONE;

    auto finalize_sm = [&](bool ignore_empty) {
        if (active_speed_modifier != size_t(-1)) {
            assert(active_speed_modifier < adjustment->lines.size());
            CoolingLine &sm = adjustment->lines[active_speed_modifier];
            // There should be at least some extrusion move inside the adjustment block.
            // However if the block has no extrusion (which is wrong), fix it for the cooling buffer to work.
            //FIXME: Pressure equalizer add EXTRUDE_SET_SPEED_TAG withotu removing the previous one at the line before.
            if (!ignore_empty && sm.length <= 0) {
                // the mouvment has been deleted because it'ts too short for the precision.
                // so soft-delete the CoolingLine (will be deleted in the apply layer cooldown func)
                sm.type = CoolingLine::TYPE_ADJUSTABLE_EMPTY;
                return;
            }
            assert(ignore_empty || sm.length > 0);
            assert(ignore_empty || sm.time > 0);
            if (sm.time <= 0) {
                // Likely a zero length extrusion, it should not be emitted, however the zero extrusions should
                // not confuse firmware either. Prohibit time adjustment of a block of zero length extrusions by
                // the cooling buffer.
                sm.type &= ~CoolingLine::TYPE_ADJUSTABLE;
                // But the start / end comment shall be removed.
                sm.type |= CoolingLine::TYPE_ADJUSTABLE_EMPTY;
            }
        }
    };

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
        else if (boost::starts_with(sline, "G2 "))
            line.type = CoolingLine::TYPE_G2;
        else if (boost::starts_with(sline, "G3 "))
            line.type = CoolingLine::TYPE_G3;
        if (line.type) {
            // G0, G1 or G92
            if (m_config.use_relative_e_distances.value)
                // Reset extruder accumulator.
                current_pos[3] = 0.f;
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
                              (*c == extrusion_axis) ? 3 : (*c == 'F') ? 4 :
                              (*c == 'I') ? 5 : (*c == 'J') ? 6 : size_t(-1);
                line.has_move = line.has_move || axis != 4;
                if (axis != size_t(-1)) {
                    new_pos[axis] = float(atof(++c));
                    if (axis == 4) {
                        // Convert mm/min to mm/sec.
                        new_pos[4] /= 60.f;
                        if ((line.type & CoolingLine::TYPE_G92) == 0) {
                            // This is G0 or G1 line and it sets the feedrate. This mark is used for reducing the duplicate F calls.
                            line.type |= CoolingLine::TYPE_HAS_F;
                        }
                    } else if (axis == 5 || axis == 6) {
                        // BBS: get position of arc center
                        new_pos[axis] += current_pos[axis - 5];
                    }
                }
                // Skip this word.
                for (; *c != ' ' && *c != '\t' && *c != 0; ++ c);
            }
            bool wipe               = boost::contains(sline, ";_WIPE");
            if (wipe)
                line.type |= CoolingLine::TYPE_WIPE;
            if (boost::contains(sline, ";_EXTRUDE_SET_SPEED") && !wipe) {
                finalize_sm(true);
                active_speed_modifier = adjustment->lines.size();
                line.type |= CoolingLine::TYPE_ADJUSTABLE;
                current_stamp |= CoolingLine::TYPE_ADJUSTABLE;
                if (boost::contains(sline, ";_EXTRUDE_SET_SPEED_MAYBE")) {
                    line.type |= CoolingLine::TYPE_ADJUSTABLE_MAYBE;
                    current_stamp |= CoolingLine::TYPE_ADJUSTABLE_MAYBE;
                }
            }
            if ((line.type & CoolingLine::TYPE_G92) == 0) {
                // G0 or G1. Calculate the duration.
                float dif[4];
                for (size_t i = 0; i < 4; ++ i)
                    dif[i] = new_pos[i] - current_pos[i];
                float dxy2 = 0;
                //BBS: support to calculate length of arc
                if (line.type & CoolingLine::TYPE_G2 || line.type & CoolingLine::TYPE_G3) {
                    Vec3f start(current_pos[0], current_pos[1], 0);
                    Vec3f end(new_pos[0], new_pos[1], 0);
                    Vec3f center(new_pos[5], new_pos[6], 0);
                    bool is_ccw = line.type & CoolingLine::TYPE_G3;
                    float dxy = Slic3r::Geometry::ArcSegment::calc_arc_length(start, end, center, is_ccw);
                    dxy2 = dxy * dxy;
                } else {
                    dxy2 = dif[0] * dif[0] + dif[1] * dif[1];
                }
                float dxyz2 = dxy2 + dif[2] * dif[2];
                if (dxyz2 > 0.f) {
                    // Movement in xyz, calculate time from the xyz Euclidian distance.
                    line.length = sqrt(dxyz2);
                } else if (std::abs(dif[3]) > 0.f) {
                    // Movement in the extruder axis.
                    line.length = std::abs(dif[3]);
                }
                line.feedrate = new_pos[4];
                if (line.feedrate > 0.f && line.length > 0.f) {
                    assert((line.type & CoolingLine::TYPE_ADJUSTABLE) == 0);
                    // there can be no active_speed_modifier in custom gcode. 
                    assert(active_speed_modifier != size_t(-1) || current_stamp == CoolingLine::TYPE_NONE);
                    line.type |= current_stamp;
                }
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
                // BBS: add G2 and G3 support
                if (active_speed_modifier < adjustment->lines.size() && 
                    ((line.type & CoolingLine::TYPE_G1) || (line.type & CoolingLine::TYPE_G2) || (line.type & CoolingLine::TYPE_G3))) {
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
            finalize_sm(false);
            active_speed_modifier = size_t(-1);
            current_stamp         = CoolingLine::TYPE_NONE;
        } else if (boost::starts_with(sline, ";_TOOLCHANGE")) {
            //not using m_toolchange_prefix anymore because there is no use case for it, there is always a _TOOLCHANGE for when a fan change is needed.
            int prefix = 13;
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
            } else {
                // Only log the error in case of MM printer. Single extruder printers likely ignore any T anyway.
                if (map_extruder_to_per_extruder_adjustment.size() > 1)
                    BOOST_LOG_TRIVIAL(error) << "CoolingBuffer encountered an invalid toolchange, maybe from a custom gcode: " << sline;
            }

        } else if (boost::starts_with(sline, ";_EXTRUDETYPE_") && sline.size() > 14) {
            //set the extrusiontype
            line.type |= CoolingLine::Type(sline[14] - 'A') | CoolingLine::Type::TYPE_EXTRUDE_START;
            assert(CoolingLine::to_extrusion_role(uint32_t(sline[14] - 'A')) != 0);
            if (CoolingLine::to_extrusion_role(uint32_t(sline[14] - 'A')) == 0) {
                line.type |= ExtrusionRole::erCustom;
            }
        } else if (boost::starts_with(sline, "G4 ")) {
            // Parse the wait time.
            line.type = CoolingLine::TYPE_G4;
            size_t pos_S = sline.find('S', 3);
            size_t pos_P = sline.find('P', 3);
            assert(is_decimal_separator_point()); // for atof
            line.time = line.time_max = float(
                (pos_S > 0) ? atof(sline.c_str() + pos_S + 1) :
                (pos_P > 0) ? atof(sline.c_str() + pos_P + 1) * 0.001 : 0.);
        } else if (boost::starts_with(sline, ";_STORE_FAN_SPEED_WT")) {
            line.type = CoolingLine::TYPE_STORE_FOR_WT;
        } else if (boost::starts_with(sline, ";_RESTORE_FAN_SPEED_WT")) {
            line.type = CoolingLine::TYPE_RESTORE_AFTER_WT;
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

// list of fan that can be increased (erNone is the default) via fan_below_layer_time and slowdown_below_layer_time
// the commented ones won't buldge even is the layer time is low.
std::vector<uint8_t> etype_can_increase_fan = {
    ExtrusionRole::erNone,
    ExtrusionRole::erBridgeInfill,
    ExtrusionRole::erInternalBridgeInfill,
    //ExtrusionRole::erTopSolidInfill,
    //ExtrusionRole::erIroning,
    //ExtrusionRole::erSupportMaterialInterface,
    //ExtrusionRole::erSupportMaterial,
    ExtrusionRole::erExternalPerimeter,
    ExtrusionRole::erThinWall,
    ExtrusionRole::erPerimeter,
    ExtrusionRole::erSolidInfill,
    ExtrusionRole::erInternalInfill,
    ExtrusionRole::erOverhangPerimeter,
    ExtrusionRole::erGapFill };
// list of fan that won't be reduced in the first layers by full_fan_speed_layer (after disable_fan_first_layers)
std::vector<uint8_t> etype_can_ramp_up_fan = {
    ExtrusionRole::erNone,
    //ExtrusionRole::erBridgeInfill,
    //ExtrusionRole::erInternalBridgeInfill,
    ExtrusionRole::erTopSolidInfill,
    ExtrusionRole::erIroning,
    //ExtrusionRole::erSupportMaterialInterface,
    ExtrusionRole::erSupportMaterial,
    ExtrusionRole::erExternalPerimeter,
    ExtrusionRole::erThinWall,
    ExtrusionRole::erPerimeter,
    ExtrusionRole::erSolidInfill,
    ExtrusionRole::erInternalInfill,
    //ExtrusionRole::erOverhangPerimeter,
    ExtrusionRole::erGapFill };

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
    bool fan_control[ExtrusionRole::erCount];
    int fan_speeds[ExtrusionRole::erCount];
    int default_fan_speed[ExtrusionRole::erCount];
#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_current_extruder)
    const int min_fan_speed             = m_config.fan_printer_min_speed;
    assert(min_fan_speed >= 0);
    int initial_default_fan_speed = EXTRUDER_CONFIG(default_fan_speed);
    //if default_fan_speed activated, be sure it's at least the mins
    if (initial_default_fan_speed > 0 && initial_default_fan_speed < min_fan_speed)
        initial_default_fan_speed = min_fan_speed;
    // 0 was deprecated, replaced by 1: allow 1 to still be 0 (and it's now deprecated)
    if (initial_default_fan_speed == 1)
        initial_default_fan_speed = 0;
    //initialise the speed array
    for (int i = 0; i < ExtrusionRole::erCount; i++) {
        fan_control[i] = false;
        fan_speeds[i] = 0;
        default_fan_speed[i] = initial_default_fan_speed;
        // 0 was deprecated, replaced by 1: allow 1 to still be 0 (and it's now deprecated)
        if (default_fan_speed[i] == 1) default_fan_speed[i] = 0;
    }
    //set the fan controls
    default_fan_speed[ExtrusionRole::erBridgeInfill] = EXTRUDER_CONFIG(bridge_fan_speed);
    default_fan_speed[ExtrusionRole::erInternalBridgeInfill] = EXTRUDER_CONFIG(internal_bridge_fan_speed);
    default_fan_speed[ExtrusionRole::erTopSolidInfill] = EXTRUDER_CONFIG(top_fan_speed);
    default_fan_speed[ExtrusionRole::erIroning] = default_fan_speed[ExtrusionRole::erTopSolidInfill];
    default_fan_speed[ExtrusionRole::erSupportMaterialInterface] = EXTRUDER_CONFIG(support_material_interface_fan_speed);
    default_fan_speed[ExtrusionRole::erSupportMaterial] = EXTRUDER_CONFIG(support_material_fan_speed);
    default_fan_speed[ExtrusionRole::erExternalPerimeter] = EXTRUDER_CONFIG(external_perimeter_fan_speed);
    default_fan_speed[ExtrusionRole::erThinWall] = default_fan_speed[ExtrusionRole::erExternalPerimeter];
    default_fan_speed[ExtrusionRole::erPerimeter] = EXTRUDER_CONFIG(perimeter_fan_speed);
    default_fan_speed[ExtrusionRole::erSolidInfill] = EXTRUDER_CONFIG(solid_infill_fan_speed);
    default_fan_speed[ExtrusionRole::erInternalInfill] = EXTRUDER_CONFIG(infill_fan_speed);
    default_fan_speed[ExtrusionRole::erOverhangPerimeter] = EXTRUDER_CONFIG(overhangs_fan_speed);
    default_fan_speed[ExtrusionRole::erGapFill] = EXTRUDER_CONFIG(gap_fill_fan_speed);
    // if default is enabled, it takes over the settings that are disabled.
    if (initial_default_fan_speed >= 0) {
        for (int i = 0; i < ExtrusionRole::erCount; i++) {
            // this setting is disbaled. As default is not, it will use the default value
            if (default_fan_speed[i] < 0) {
                default_fan_speed[i] = initial_default_fan_speed;
            }
        }
    }
    auto change_extruder_set_fan = [this, layer_id, layer_time, &new_gcode, 
            &fan_control, &fan_speeds, &default_fan_speed, initial_default_fan_speed, min_fan_speed]()
    {
        int disable_fan_first_layers = EXTRUDER_CONFIG(disable_fan_first_layers);
        // Is the fan speed ramp enabled?
        int full_fan_speed_layer = EXTRUDER_CONFIG(full_fan_speed_layer);
        if (int(layer_id) >= disable_fan_first_layers) {
            int   max_fan_speed             = EXTRUDER_CONFIG(max_fan_speed);
            float slowdown_below_layer_time = float(EXTRUDER_CONFIG(slowdown_below_layer_time));
            float fan_below_layer_time      = float(EXTRUDER_CONFIG(fan_below_layer_time));
            for (int i = 0; i < ExtrusionRole::erCount; i++) {
                fan_speeds[i] = default_fan_speed[i];
            }
            //fan_speeds[0] carry the current default value. ensure it's not negative.
            if (initial_default_fan_speed <= 0) {
                fan_speeds[0] = 0;
            }
            if (layer_time < slowdown_below_layer_time && fan_below_layer_time > 0) {
                // Layer time very short. Enable the fan to a full throttle.
                //fan_speed_new = std::max(max_fan_speed, fan_speed_new);
                for (size_t etype_idx = 0; etype_idx < etype_can_increase_fan.size(); etype_idx++) {
                    uint16_t idx = etype_can_increase_fan[etype_idx];
                    fan_speeds[idx] = std::max(max_fan_speed, fan_speeds[idx]);
                }
            } else if (layer_time < fan_below_layer_time) {
                // Layer time quite short. Enable the fan proportionally according to the current layer time.
                assert(layer_time >= slowdown_below_layer_time);
                double t = (layer_time - slowdown_below_layer_time) / (fan_below_layer_time - slowdown_below_layer_time);
                for (size_t etype_idx = 0; etype_idx < etype_can_increase_fan.size(); etype_idx++) {
                    uint16_t idx = etype_can_increase_fan[etype_idx];
                    if (fan_speeds[idx] < max_fan_speed) // if max speed is lower, this will reduce speed, so don't do it.
                        fan_speeds[idx] = std::clamp(int(t * fan_speeds[idx] + (1. - t) * max_fan_speed + 0.5), 0, 255);
                }
            }

            // Is the fan speed ramp enabled?
            int full_fan_speed_layer = EXTRUDER_CONFIG(full_fan_speed_layer);
            // When ramping up fan speed from disable_fan_first_layers to full_fan_speed_layer, if disable_fan_first_layers is zero,
            // the not-fan layer is a hypothetical -1 layer.
            if (int(layer_id) >= disable_fan_first_layers && int(layer_id) + 1 < full_fan_speed_layer) {
                // Ramp up the fan speed from disable_fan_first_layers to full_fan_speed_layer.
                float factor = float(int(layer_id + 1) - disable_fan_first_layers) / float(full_fan_speed_layer - disable_fan_first_layers);
                for (size_t etype_idx = 0; etype_idx < etype_can_ramp_up_fan.size(); etype_idx++) {
                    uint16_t idx = etype_can_ramp_up_fan[etype_idx];
                    fan_speeds[idx] = std::clamp(int(float(fan_speeds[idx]) * factor + 0.01f), 0, 255);
                }
            }
            //only activate fan control if the fan speed is higher than min
            fan_control[0] = fan_speeds[0] >= 0;
            for (size_t i = 1; i < ExtrusionRole::erCount; i++) {
                fan_control[i] = fan_speeds[i] >= 0;
            }

            // if bridge_fan is disabled, it takes the value of default_fan
            if (!fan_control[ExtrusionRole::erBridgeInfill] && fan_control[0]) {
                fan_control[ExtrusionRole::erBridgeInfill] = true;
                fan_speeds[ExtrusionRole::erBridgeInfill] = fan_speeds[0];
            }

            // if internal_bridge_fan is disabled, it takes the value of bridge_fan
            if (!fan_control[ExtrusionRole::erInternalBridgeInfill] && fan_control[ExtrusionRole::erBridgeInfill]) {
                fan_control[ExtrusionRole::erInternalBridgeInfill] = true;
                fan_speeds[ExtrusionRole::erInternalBridgeInfill] = fan_speeds[ExtrusionRole::erBridgeInfill];
            }

            // if external_perimeter_fan is disabled, it takes the value of perimeter_fan
            if (!fan_control[ExtrusionRole::erExternalPerimeter] && fan_control[ExtrusionRole::erPerimeter]) {
                fan_control[ExtrusionRole::erExternalPerimeter] = true;
                fan_speeds[ExtrusionRole::erExternalPerimeter] = fan_speeds[ExtrusionRole::erPerimeter];
            }

            // if top_fan is disabled, it takes the value of solid_fan
            if (!fan_control[ExtrusionRole::erTopSolidInfill] && fan_control[ExtrusionRole::erSolidInfill]) {
                fan_control[ExtrusionRole::erTopSolidInfill] = true;
                fan_speeds[ExtrusionRole::erTopSolidInfill] = fan_speeds[ExtrusionRole::erSolidInfill];
            }

        } else {
            fan_control[0] = true;
            fan_speeds[0] = 0;
            for (int i = 1; i < ExtrusionRole::erCount; i++) {
                fan_control[i] = false;
                fan_speeds[i] = 0;
            }
        }
        // apply min fan speed, after the eventual speedup.
        for (int i = 1; i < ExtrusionRole::erCount; i++) {
            if (fan_control[i] && fan_speeds[i] > 0) {
                fan_speeds[i] = std::max(fan_speeds[i], min_fan_speed);
            }
        }
        if (fan_speeds[0] != m_fan_speed && fan_control[0]) {
            m_fan_speed = fan_speeds[0];
            new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, m_fan_speed,
                                              EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,
                                              std::string("set fan for new extruder"));
        }
    };
    //set to know all fan modifiers that can be applied ( TYPE_BRIDGE_FAN_END, TYPE_TOP_FAN_START, TYPE_SUPP_INTER_FAN_START, TYPE_EXTERNAL_PERIMETER).
    std::vector<ExtrusionRole> extrude_tree;
    const char         *pos               = gcode.c_str();
    int                 current_feedrate  = 0;
    int                 stored_fan_speed  = m_fan_speed < 0 ? 0 : m_fan_speed;
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
        } else if (line->type & CoolingLine::TYPE_STORE_FOR_WT) {
            stored_fan_speed = m_fan_speed < 0 ? 0 : m_fan_speed;
        } else if (line->type & CoolingLine::TYPE_RESTORE_AFTER_WT) {
            new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, stored_fan_speed,
                                              EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,
                                              "restore fan after wipe tower");
        } else if (line->type & CoolingLine::TYPE_EXTRUDE_START) {
            assert(CoolingLine::to_extrusion_role(uint32_t(line->type)) != 0);
            extrude_tree.push_back(CoolingLine::to_extrusion_role(uint32_t(line->type)));
            fan_need_set = true;
        } else if (line->type & CoolingLine::TYPE_EXTRUDE_END) {
            assert(extrude_tree.size() > 0);
            if (extrude_tree.size() > 0) {
                extrude_tree.pop_back();
                fan_need_set = true;
            }
        } else if (line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_ADJUSTABLE_EMPTY | CoolingLine::TYPE_ADJUSTABLE_MAYBE | CoolingLine::TYPE_WIPE | CoolingLine::TYPE_HAS_F)) {

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
                if ((line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_ADJUSTABLE_EMPTY | CoolingLine::TYPE_ADJUSTABLE_MAYBE | CoolingLine::TYPE_WIPE)) || !line->has_move )
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
                if (line->type & (CoolingLine::TYPE_ADJUSTABLE | CoolingLine::TYPE_ADJUSTABLE_EMPTY | CoolingLine::TYPE_ADJUSTABLE_MAYBE | CoolingLine::TYPE_WIPE)) {
                    // Process comments, remove ";_EXTRUDE_SET_SPEED", ";_EXTRUDE_SET_SPEED_MAYBE", ";_WIPE"
                    std::string comment(end, line_end);
                    if (line->type & (CoolingLine::TYPE_ADJUSTABLE_MAYBE)) {
                        boost::replace_all(comment, ";_EXTRUDE_SET_SPEED_MAYBE", "");
                    } else {
                        boost::replace_all(comment, ";_EXTRUDE_SET_SPEED", "");
                    }
                    if (line->type & CoolingLine::TYPE_WIPE)
                        boost::replace_all(comment, ";_WIPE", "");
                    new_gcode += comment;
                } else {
                    // Just attach the rest of the source line.
                    new_gcode.append(end, line_end - end);
                }
            }
        } else if(line->type == CoolingLine::TYPE_ADJUSTABLE_EMPTY) {
            // nothing useful, don't write it (an extrusion that don't move because it wasn't printed as it's too small).
            std::string deleted(line_start, line_end - line_start);
            boost::replace_all(deleted, "\n", "");
            new_gcode.append(std::string("; deleted stuff: ") + deleted);
        } else {
            new_gcode.append(line_start, line_end - line_start);
        }
        if (fan_need_set) {
            //use the most current fan
            bool fan_set = false;
            for (size_t i = extrude_tree.size() - 1; i < extrude_tree.size(); --i) {
                if (fan_control[extrude_tree[i]]) {
                    new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments,
                                                      fan_speeds[extrude_tree[i]],
                                                      EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,
                                                      std::string("set fan for ") + ExtrusionEntity::role_to_string(extrude_tree[i]));
                    fan_set = true;
                    break;
                }
            }
            if (!fan_set) {
                // return to default
                new_gcode += GCodeWriter::set_fan(m_config.gcode_flavor, m_config.gcode_comments, m_fan_speed < 0 ? 0 : m_fan_speed,
                                                    EXTRUDER_CONFIG(extruder_fan_offset), m_config.fan_percentage,
                                                    "set default fan");
            }
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
