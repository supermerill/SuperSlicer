///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2020 - 2023 David Kocík @kocikdav, Enrico Turri @enricoturri1966, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_GUI_Init_hpp_
#define slic3r_GUI_Init_hpp_

#include <memory>

#include "libslic3r/Config/Preset.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
namespace Slic3r {

class AppConfig;

namespace GUI {

struct OpenGLVersions
{
	static const std::vector<std::pair<int, int>> core;
};

struct GUI_InitParams
{
	int		                    argc;
	char	                  **argv;

	// Substitutions of unknown configuration values done during loading of user presets.
	PresetsConfigSubstitutions  preset_substitutions;

    std::vector<std::string>    load_configs;
    DynamicPrintConfig          extra_config;
    std::vector<std::string>    input_files;

    bool                        start_as_gcodeviewer;
    bool                        start_downloader;
    bool                        delete_after_load;
    std::string                 download_url;
    std::unique_ptr<AppConfig>  app_config;
#if ENABLE_GL_CORE_PROFILE
		std::pair<int, int>         opengl_version;
		bool                        opengl_debug;
		bool                        opengl_compatibiity_profile;
#endif // ENABLE_GL_CORE_PROFILE
};

int GUI_Run(GUI_InitParams &params);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Init_hpp_
