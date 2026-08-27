///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Pavel Mikuš @Godrak, Lukáš Hejl @hejllukas, Filip Sykala @Jony01, Enrico Turri @enricoturri1966, David Kocík @kocikdav, Oleksandra Iushchenko @YuSanka
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) 2019 Thomas Moore
///|/ Copyright (c) 2016 Chow Loong Jin @hyperair
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/
///|/ ported from lib/Slic3r/GCode.pm:
///|/ Copyright (c) Slic3r 2011 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2013 Robert Giseburt
///|/ Copyright (c) 2012 Mark Hindess
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "HFP.hpp"

#include <exception>
#include <fstream>
#include <sstream>

#include <boost/log/trivial.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Config/PrintConfig.hpp"

namespace Slic3r {

HFP::HFP() : m_base_layer_height(0.0), m_layer_height(0.0) {}

bool HFP::valid_hfp() const { return m_json_data.is_object(); }

bool HFP::load_hfp(const std::string &input_file) {
    std::ifstream file(input_file);

    if (!file.is_open()) {
        BOOST_LOG_TRIVIAL(error) << "Failed to open HFP file: " << input_file;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();                  // Read the entire file into a buffer
    std::string file_content = buffer.str(); // Convert buffer to a string
    file.close();                            // Close file after reading
    Filament filament;
    BOOST_LOG_TRIVIAL(info) << "Processing HFP file: " << input_file;

    // reset
    m_filament_set.clear();
    m_json_data.clear();
    m_slider_values.clear();
    m_base_layer_height = 0.0;
    m_layer_height = 0.0;

    try {
        // Check if content is JSON format
        if (file_content.find("{") != std::string::npos) {
            BOOST_LOG_TRIVIAL(info) << "Detected JSON format.";
            m_json_data = nlohmann::json::parse(file_content);

            // Load stl path
            m_stl_path = m_json_data.value("stl", "");

            // Load Base Layer Height
            if (m_json_data.contains("base_layer_height")) {
                m_base_layer_height = m_json_data.value("base_layer_height", 0.0);
                BOOST_LOG_TRIVIAL(info) << "Base Layer Height: " << m_base_layer_height;
            }

            // Load Layer Height
            if (m_json_data.contains("layer_height")) {
                m_layer_height = m_json_data.value("layer_height", 0.0);
                BOOST_LOG_TRIVIAL(info) << "Layer Height: " << m_layer_height;
            }

            // Load Filament Set
            if (m_json_data.contains("filament_set") && m_json_data["filament_set"].is_array()) {
                BOOST_LOG_TRIVIAL(info) << "Loading filament set from JSON...";
                m_filament_set.clear();

                for (const auto &filament_json : m_json_data["filament_set"]) {
                    Filament filament;
                    filament.Brand = filament_json.value("Brand", "");
                    filament.Color = filament_json.value("Color", "");
                    filament.Name = filament_json.value("Name", "");
                    filament.Owned = filament_json.value("Owned", false);
                    filament.Transmissivity = filament_json.value("Transmissivity", 0.0);
                    filament.Type = filament_json.value("Type", "");
                    filament.uuid = filament_json.value("uuid", "");

                    m_filament_set.push_back(filament);
                    BOOST_LOG_TRIVIAL(info) << "Loaded filament: " << filament.Brand << " (" << filament.Name << ")";
                }
            } else {
                BOOST_LOG_TRIVIAL(warning) << "No 'filament_set' found in JSON.";
            }

            // Load Slider Values
            if (m_json_data.contains("slider_values") && m_json_data["slider_values"].is_array()) {
                BOOST_LOG_TRIVIAL(info) << "Loading slider values...";
                m_slider_values.clear();

                for (const auto &value : m_json_data["slider_values"]) {
                    int slider_value = value.get<int>() + 1; // Always increase by 1
                    m_slider_values.push_back(slider_value);
                }

                BOOST_LOG_TRIVIAL(info) << "Slider values loaded with +1 increment.";
            } else {
                BOOST_LOG_TRIVIAL(warning) << "No 'slider_values' found in JSON.";
            }
        }

        // Otherwise, process as key-value pairs (plain text format)
        else {
            BOOST_LOG_TRIVIAL(info) << "Detected Key-Value format.";
            std::istringstream stream(file_content);
            std::string line;

            while (std::getline(stream, line)) {
                BOOST_LOG_TRIVIAL(info) << "Processing line: " << line;

                size_t delimiter_pos = line.find(":");
                if (delimiter_pos != std::string::npos) {
                    std::string key = line.substr(0, delimiter_pos);
                    std::string value = line.substr(delimiter_pos + 1);

                    // Trim spaces
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t") + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    value.erase(value.find_last_not_of(" \t") + 1);

                    // Process each key dynamically
                    if (key == "base_layer_height") {
                        m_base_layer_height = std::stof(value);
                        BOOST_LOG_TRIVIAL(info) << "Base Layer Height: " << m_base_layer_height;
                    } else if (key == "layer_height") {
                        m_layer_height = std::stof(value);
                        BOOST_LOG_TRIVIAL(info) << "Layer Height: " << m_layer_height;
                    } else if (key == "slider_values") {
                        std::istringstream value_stream(value);
                        int slider_value;
                        while (value_stream >> slider_value) {
                            m_slider_values.push_back(slider_value + 1); // Always increment by 1
                        }
                        BOOST_LOG_TRIVIAL(info) << "Loaded slider values with +1 increment.";
                    }
                    // Handle filaments dynamically (if stored as key-value in this format)
                    else if (key.find("filament_") == 0) {
                        filament.Name = key;
                        filament.Brand = value;
                        m_filament_set.push_back(filament);
                        BOOST_LOG_TRIVIAL(info) << "Loaded filament: " << filament.Brand;
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Error processing HFP file: " << e.what();
        return false;
    }
    return true;
}

void HFP::set_custom_gcode_z(Model &model) const {
    model.custom_gcode_per_print_z.gcodes.clear();

    int extruder = 1;
    CustomGCode::Type type = CustomGCode::ColorChange;
    std::string extra;

    if (!m_filament_set.empty()) {
        for (int i = 0; i < m_filament_set.size(); i++) {
            model.custom_gcode_per_print_z.gcodes.push_back(CustomGCode::Item{scale_to_layer_coord(m_layer_height * m_slider_values[i]),
                                                                              type, extruder, m_filament_set[i].Color,
                                                                              extra});
        }
    }
    return;
}

void HFP::update_config(DynamicPrintConfig &fff_print_config) const {
    fff_print_config.set_key_value("layer_height", new ConfigOptionFloat(this->get_layer_height()));
    fff_print_config.set_key_value("first_layer_height",
                                   new ConfigOptionFloatOrPercent(this->get_base_layer_height(), false));
}


} // namespace Slic3r
