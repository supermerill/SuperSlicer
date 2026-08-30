
# libslic3r api
These header files contains the object to extends to create a new plugin.
currently work_in_progress

## directory layout

`Api/plugin/c` contains the C ABI that plugins may include directly.
`Api/plugin/cpp` contains C++ helper views and base classes built on top of
the C ABI. `Api/plugin/python` is reserved for future Python bindings.

`Api/host` contains host-side implementation details compiled into the main
program. `Api/internal` contains lower-level accessors to native slic3r data
structures. Plugins should not depend on either directory. The `Plugins`
directory contains official plugin implementations that consume the public
plugin API.

Developer guides:

- [Using Plugin Properties](../../../doc/plugins/properties.md)

note: items with tag Planned are currently ideas that are not implemented and so may not work and may need extensive modifications.
Other items are already implemented, but may need some modifications in the future to fit into the framework

## how it works
The libsli3r contains structure with the slicing data.
Each plugin implements process that allow to create/modify these data to go to the next stages.
the slicer will call the activated process one after another to create the gcode.

## data

Main data Objects:
 * Print
 * Model
 * ModelObject
 * Object
 * PrintRegion
 * Layer
 * LayerRegion
 * LayerSliceIsland
 * LayerRegionIsland
 * Surface
for gcode cgeneration:
 * PrintOrdering
 * LayerExtusion

Some fields in the data objects may not be modifiable after a certain step. If a process call a setter method when it's not allowed anymore, an exception is emitted. This allow to use some pointers safely, as the object are now ensured to not be erased.

### Print
Entry point, there is at most one print object per plater (There may be multiple Print object in the slicer, if they choose to have multiple pater and one Print per plater).
The Print object contains the pointer to all other data.
The print contains the objects, which are independent with each other.
Model & ModelObject are the 3D inputs

### Object

TO FILL
has only one Print pointer.


### Layer
A layer of a object. It's defined by its object, the print z and the print height (and the bottom z can be computed from these two).
it contains the Expolygons slice, as it got from the slicing.
It contains a list of LayerSliceIsland and a list or LayerRegion.
It has only one Object pointer.

Note: support are now normal layers (in Slic3r and derivative, they are specials) and support extrusions can be in normal layers if the printz & printheight are the same.

Note2: for not-planar printing, the layer are pre-deformed. the deformation is saved for renormalizattion after the slicing, but the layers are still flat in all the steps in the slicer.

### LayerSliceIsland

Contains an ExPolygon, a list of LayerRegionIsland.
Contains a list of LayerSliceIsland's pointer from above and a list of LayerSliceIsland's pointer from below. All of these have their ExPolygon overlap our one.
Has only one Layer pointer



### PrintRegion

A region. It may have modified settings, and modified properties. A print can contains many regions.

### LayerRegion

Contains a list of ExPolygon (its slice)
Contains a list of Surface
Has a Layer pointer, a PrintRegion pointer


A sliced region on this layer.
LayerRegion can't overlap each other?

### LayerRegionIsland

The most precise data holder.
It is included inside a LayerSliceIsland.
It is inside at least one LayerRegion. It can have multiple one if they have the same properties needed for a given process.
It is set for only one extruder.
It contains the extrusion to be printed, stored by broad category (perimeter, infill, support, ...).

It should only contains a single ExPolygon, that may be the same or smaller than the LayerSliceIsland.
If you intersect the LayerRegion and the LayerSliceIsland, you may have multiple ExPolygons. In this case, you need to create a LayerRegionIsland for each sub-island. 

Most of the time, when you want to add some new extrusions in the data store, you have to request for a compatible LayerRegionIsland. If it doesn't exists, then it will be created.

### Surface

A ExPolygon with surface types (top, bottom, solid infill, etc..) and some other properties.


### PrintOrdering
Planned
it's an ordering of LayerRegionIsland that only increase in layer_z
it contains a list of OrderingLayer, each one with a layer_z.

An OrderingLayer has a list of set<<LayerRegionIsland, LayerSliceIsland>>
We can get the multiple Layers (some may be from different objects) it touches from the LayerSliceIsland.
We can get the multiple regions (some may be from different objects) it touches from the LayerRegionIsland.

It also have a list of LayerExtusion to be filled by the LayerRegionIsland extrusions


LayerRegionIsland are immutable.
LayerExtusion are mutable.

### LayerExtusion
Planned
An List of <ExtrusionEntity, const LayerRegionIsland*> to be printed.
The extrusionEntiy are mutable, the LayerRegionIsland is for reference, to get to the region's settings.



## steps

stages are listed in their order.
A step can be unique or multiple. A unique step means that only one process can be registered for it, else it can have multiple ones. In this case, their order can be random or defined from process preferences.

### slicing
Unique
input: Print, Model, ModelObject, Object
output: Layer, LayerSliceIsland, LayerRegion

Slice the parts of the modelobject to create the Layers of the object.
Slice the modifiers of the modelobject to create the LayerRegions of the object.
Slice other volumes of the modelobject to be able to use them afterwards (brim, seam, support, etc)
assign an extruder to each region.

### slicing modifications
Multiple
input : Print, Object, Layer, LayerSliceIsland, LayerRegion
output: Layer, LayerSliceIsland, LayerRegion

Modify the slices from the slicing. It's a bit like modifying the input geometry.

current: 
holes to polyholes
max overhangs
curve smoothing

### surface creation
Unique
create surfaces from Islands 

### Perimeter Generator pre-process
Multiple
for things like "avoid overhangs on bridges area"

### Perimeter Generator
Unique
input: Layer, LayerSliceIsland, LayerRegion, Surface
output: Layer, LayerSliceIsland, LayerRegion, Surface, LayerRegionIsland

Create perimeters.
It create LayerRegionIsland as needed to store the perimeter extrusions.
It modify the surfaces so the infill surfaces don't overlap with perimeter.

currently, the core algorithm takes a single surface and output perimeter extrusions & infill polygons from it. The process do the piping around it.

### prepare_infill
multiple
input: Layer, LayerSliceIsland, LayerRegion, Surface
output: Layer, LayerSliceIsland, LayerRegion, Surface

modify the surfaces and their tag so it's possible to infill them with the right process at the right spot.
detect the top, bottom, the void, the sparse, bridge areas.

exemples of current process list in use ('ensure vertical shell thickness' changes it):
detect_surfaces_type->prepare_fill_surfaces->process_external_surfaces->discover_horizontal_shells
detect_surfaces_type->prepare_fill_surfaces->discover_vertical_shells->process_external_surfaces

### prepare_infill_post_process
multiple
input: Layer, LayerSliceIsland, LayerRegion, Surface
output: Layer, LayerSliceIsland, LayerRegion, Surface

alter surfaces from prepare_infill for various purpose.

current post-processes:
clean_surfaces
tag_under_bridge
bridge_over_infill
combine_infill
compute_max_sparse_spacing

### infill
Multiple
input: Layer, LayerSliceIsland, LayerRegion, Surface
output: Layer, LayerSliceIsland, LayerRegion, Surface, LayerRegionIsland

Infill a surface and output the extrusions result into a LayerRegionIsland.

note:
It combines same surfaces of LayerRegion with same properties to have the biggest surface possible.

current infill processes:
 * 'normal' infill
 * ironing

### Generate Support Spots

Detect surfaces that are problematic and should be supported.
TODO: currently this algorithm is on the side. It should be incorporated into the main workflow and maybe used as input for the "create support"

### Create support
Unique
input: Object, Layer, LayerSliceIsland, LayerRegion, Surface
output: Object, Layer, LayerSliceIsland, LayerRegion, Surface, LayerRegionIsland

create support:
can create layers
can create extrusions

### Slicing post process
Multiple
input: Print, Object, Layer, LayerSliceIsland, LayerRegion, Surface, LayerRegionIsland
output: Print, Object, Layer, LayerSliceIsland, LayerRegion, Surface, LayerRegionIsland

process to create data or extrusions before everything is set in stone for the sorting.

current:
estimate_curled_extrusions
calculate_overhanging_perimeters
create brim and skirt (they need to be created into a LayerRegionIsland like everyone else, but this one can be special)
create arcs

## Ordering
Unique
input: Print, Object, Layer, LayerSliceIsland, LayerRegion, Surface, LayerRegionIsland
Output: PrintOrdering

Order the LayerRegionIsland for printing.

there is multiple ordering algorithm.
The normal one first sort all layers from all objects by print_z, then in each group by extruder, then islands by proximity (or not at all, ready to be sorted on extrusion proximity)
The parallel object sort by object first, then by layers, extruder, island.
The parallel step is a combination of both.

The output Ordering object keep the ordering

## wipetower
Unique
Input: LayerRegionIsland, PrintOrdering
Output: LayerRegionIsland, PrintOrdering

Create wipetower(s). It uses the wipetowers model/volume from the print/ObjectModel.
There is one wipetower if normal printing, there is one wipetower per object if parallel objects printing (Planned).

Using the ordering, the wipetower toolchanges are planned.
As there may be extra wipes, the wipetower size & extrusion can't be computed right now, so the extrusions added into the Ordering are placeholders.

## Layer stiching
Unique
Input: PrintOrdering
Output: PrintOrdering, LayerExtusion

go over each layer to decide the order of each extrusion. Choose seams.
Should be made sequentially to use the previous layer position to minimise travel.
To prevent bottleneck, the layers already computed can be passed in the next steps via pipelining, until one process need to have everything synchronized (like the result of compute layer time)

## extrusion post-processing
Multiple
Input: PrintOrdering, LayerExtusion
Output: PrintOrdering, LayerExtusion

modify the extrusions or their order

The GcodeWriter need to be changed into an utility to traverse the extrusions to be able to easily see the current state.

Note: most now can be done in // (by layer)

current:
seam notch
loop cut (add seam gap)
generate speed & acceleration (by region -> Planned)
generate retract & lift
generate retract-wipe
generate wipe-tower wipe (prevent filament grinding by too much retraction on the same spot) (Planned)
generate heating
generate fan speed
compute layer time
min layer time
heating advance (Planned)
fan speed advance

## generate gcode
UNIQUE
Input: Ordering, LayerExtusion
output: gcode file
from the extrusion, generate gcode
note: can now be done in //
