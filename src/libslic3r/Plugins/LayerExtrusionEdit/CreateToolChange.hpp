///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_CreateToolChange_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_CreateToolChange_hpp_

/*
Register the built-in tool-selection producer.

Purpose
-------

The provider materializes every real tool change in the ordered PrintingPlan
before transition scopes are created. A configured toolchange_gcode becomes a
PlaceholderParser script. When that setting contains no visible text, the
provider inserts the semantic TOOLCHANGE command understood by firmware
sessions instead.

Input requirements
------------------

- PrintingGroup, PrintingLayerGroup and PrintingToolGroup order must already
  be final.
- Tool-group extruder IDs must identify real tools.
- Tool-group event roots must still accept ordered additions.

Output contract
---------------

- The first selected tool remains implicit and is chosen by the firmware when
  its first tool-group begins.
- Every later change is represented exactly once in the target tool-group's
  before events, including changes introduced by an empty tool-group.
- A non-empty script is solely responsible for physical selection; no semantic
  fallback is stored beside it.
- Extrusion trees are not inspected or modified. CreateTransitionScope may
  subsequently detect the same tool boundary for retraction policy without
  becoming responsible for selecting the tool.
*/

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace CreateToolChangePlugin {

/* Register the built-in provider in one orchestrator. */
void register_create_tool_change_plugin(orchestrator_handle *orchestrator);

}}} // namespace slic3r_api::LayerExtrusionEdit::CreateToolChangePlugin

#endif // slic3r_Plugins_LayerExtrusionEdit_CreateToolChange_hpp_
