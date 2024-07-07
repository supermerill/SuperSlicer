#include "SpiralVase.hpp"
#include "LocalesUtils.hpp"
#include "GCode.hpp"
#include "GCodeProcessor.hpp"
#include <sstream>

namespace Slic3r {

std::string SpiralVase::process_layer(const std::string &gcode)
{
    /*  This post-processor relies on several assumptions:
        - all layers are processed through it, including those that are not supposed
          to be transformed, in order to update the reader with the XY positions
        - each call to this method includes a full layer, with a single Z move
          at the beginning
        - each layer is composed by suitable geometry (i.e. a single complete loop)
        - loops were not clipped before calling this method  */

    // If we're not going to modify G-code, just feed it to the reader
    // in order to update positions.
    if (! m_enabled) {
        m_reader.parse_buffer(gcode);
        return gcode;
    }

    // Get total XY length for this layer by summing all extrusion moves.
    float total_layer_length = 0;
    float layer_height = 0;
    float z = 0.f;
    std::string height_str = "";
    {
        //FIXME Performance warning: This copies the GCodeConfig of the reader.
        GCodeReader r = m_reader;  // clone
        bool set_z = false;
        bool milling = false;
        r.parse_buffer(gcode, [&total_layer_length, &layer_height, &z, &set_z, &height_str, &milling]
            (GCodeReader &reader, const GCodeReader::GCodeLine &line) {
            if (boost::starts_with(line.comment(), " milling"))
                milling = true;
            if (!milling) {
                if (line.cmd_is("G1")) {
                    if (line.extruding(reader)) {
                        total_layer_length += line.dist_XY(reader);
                    } else if (line.has(Z)) {
                        layer_height += line.dist_Z(reader);
                        if (!set_z) {
                            z = line.new_Z(reader);
                            set_z = true;
                        }
                    }
                } else {
                    const std::string& comment = line.raw();
                    if (comment.length() > 2 && comment.front() == ';') {
                        std::string comment_str = comment.substr(1);
                        if (boost::starts_with(comment_str, GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height))) {
                            height_str = comment_str.substr(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).size());
                        }
                    }
                }
            }
        });
    }

    // Remove layer height from initial Z.
    z -= layer_height;

    std::string new_gcode;
    //FIXME Tapering of the transition layer only works reliably with relative extruder distances.
    // For absolute extruder distances it will be switched off.
    // Tapering the absolute extruder distances requires to process every extrusion value after the first transition
    // layer.
    if (m_transition_layer) {
        new_gcode += "; Began spiral\n";
        if (!m_config.use_relative_e_distances.value) {
            new_gcode += "G92 E0\n";
        }
        // remove constant height and replace by constant width
        if (!height_str.empty()) {
            new_gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + "0\n";
        }
    }
    bool  keep_first_travel = m_transition_layer;
    float layer_height_factor = layer_height / total_layer_length;
    float len = 0.f;
    double E_accumulator = 0;
    double last_old_E = 0;
    bool is_milling = false;
    GCodeReader::GCodeLine line_last_position;
    m_reader.parse_buffer(gcode, [this, &keep_first_travel , &new_gcode, &z, total_layer_length, layer_height_factor, &len, &E_accumulator, &last_old_E, &height_str, &is_milling, &line_last_position]
        (GCodeReader &reader, GCodeReader::GCodeLine line) {
        if (boost::starts_with(line.comment()," milling"))
            is_milling = true;
        if (!is_milling) {
            if (line.cmd_is("G1")) {
                if (line.has_z()) {
                    // If this is the initial Z move of the layer, replace it with a
                    // (redundant) move to the last Z of previous layer.
                    line.set(reader, Z, z);
                    new_gcode += line.raw() + '\n';
                    return;
                } else {
                    float dist_XY = line.dist_XY(reader);
                    if (dist_XY > 0) {
                        // horizontal move
                        if (line.extruding(reader)) {
                            keep_first_travel = false;
                            len += dist_XY;
                            line.set(reader, Z, z + len * layer_height_factor);
                            if (m_transition_layer && line.has(E)) {
                                // Transition layer, modulate the amount of extrusion from zero to the final value.
                                if (m_config.use_relative_e_distances.value) {
                                    line.set(reader, E, line.value(E) * len / total_layer_length);
                                } else {
                                    last_old_E = line.value(E);
                                    E_accumulator += line.dist_E(reader) * len / total_layer_length;
                                    line.set(reader, E, E_accumulator);
                                }
                            }
                            new_gcode += line.raw() + '\n';
                        } else if (keep_first_travel) {
                            //we can travel until the first spiral extrusion
                            new_gcode += line.raw() + '\n';
                        }
                        line_last_position = line;
                        return;

                        /*  Skip travel moves: the move to first perimeter point will
                            cause a visible seam when loops are not aligned in XY; by skipping
                            it we blend the first loop move in the XY plane (although the smoothness
                            of such blend depend on how long the first segment is; maybe we should
                            enforce some minimum length?).  */
                    }
                }
            } else if (!height_str.empty()) {
                const std::string& comment = line.raw();
                if (comment.length() > 2 && comment.front() == ';') {
                    std::string comment_str = comment.substr(1);
                    if (boost::starts_with(comment_str, GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height))) {
                        //do not write it on the gcode
                        return;
                    }
                }
            }
            if (m_transition_layer && !m_config.use_relative_e_distances.value) {
                new_gcode += "; End spiral transition layer\n";
                new_gcode += "G92 E" + to_string_nozero(last_old_E, m_config.gcode_precision_e.value) + "\n";
            }
            new_gcode += line.raw() + '\n';
        } else {
            //milling, just copy
            new_gcode += line.raw() + '\n';
        }
    });
    if (m_transition_layer && !height_str.empty()) {
        //restore height/width
        new_gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + height_str +"\n";
    }
    if (is_milling) {
        //travel back to last good position.
        line_last_position.set(m_reader, Axis::E, 0);
        new_gcode += "; return to spiral location\n";
        new_gcode += line_last_position.raw() + "\n";
    }

    return new_gcode;
}

}
