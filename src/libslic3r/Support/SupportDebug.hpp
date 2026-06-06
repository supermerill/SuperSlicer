///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2023 Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_SupportCommon_hpp_
#define slic3r_SupportCommon_hpp_

namespace Slic3r {

class SupportGeneratorLayer;
class Layer;

namespace FFFSupport {

void export_print_z_polygons_to_svg(const char *path, SupportGeneratorLayer ** const layers, size_t n_layers);
void export_print_z_polygons_and_extrusions_to_svg(const char *path, SupportGeneratorLayer ** const layers, size_t n_layers, Layer& support_layer);

} // namespace FFFSupport

} // namespace Slic3r

#endif /* slic3r_SupportCommon_hpp_ */
