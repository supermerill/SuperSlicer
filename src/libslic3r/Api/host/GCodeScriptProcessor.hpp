///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_host_GCodeScriptProcessor_hpp_
#define slic3r_Api_host_GCodeScriptProcessor_hpp_

#include <memory>

#include "libslic3r/Api/plugin/c/slic3r_gcode_script.h"

/*
Host G-code script processor
============================

This facade owns one PlaceholderParser runtime for one export while keeping
that implementation out of public plugin headers. Firmware sessions borrow
only the compact raw_gcode_script_processor table returned by c_processor().
*/

namespace Slic3r {

class Print;

class GCodeScriptProcessor
{
public:
    explicit GCodeScriptProcessor(const Print &print);
    ~GCodeScriptProcessor();

    GCodeScriptProcessor(const GCodeScriptProcessor &) = delete;
    GCodeScriptProcessor &operator=(const GCodeScriptProcessor &) = delete;

    const raw_gcode_script_processor *c_processor() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r

#endif // slic3r_Api_host_GCodeScriptProcessor_hpp_
