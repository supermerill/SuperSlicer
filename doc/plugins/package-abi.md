# Package ABI Compatibility

Every installable plugin package contains a generated `version.ini`:

```ini
[plugin]
package_version = 1.0.0
slicer_version = 2.7.63.0-alpha+UNKNOWN

[abi]
slic3r_plugin_types.h = 1.0
slic3r_extrusion_entity.h = 1.0
steps/slic3r_step_post_perimeter.h = 1.0
```

The slicer version records the build environment, not compatibility. It remains
part of archive and cache identifiers. ABI keys are literal, case-sensitive
paths relative to the public C API directory, not numeric contract IDs.

The vtable contract `slic3r_plugin_types.h` is mandatory. Required major versions
must match the host; required minor versions must not exceed it. `0.0` denotes
an unused contract. Unknown nonzero requirements, duplicate keys, invalid
versions and missing ABI declarations are incompatible.

Native packages embed a pointer-free, versioned ASCII ABI record through
`slic3r_plugin_register_version.h`, using the same header-version macros as the
DLL export. Packaging extracts it without loading the library and fails if it
is absent, malformed, truncated or ambiguous. Keep this header last in the
export translation unit so all used C contracts are included.

The Python binding generator declares its complete C surface, including header
dependencies. The runtime package combines these requirements with its native
ones; repository Python packages conservatively require the whole generated
surface. This does **not** detect changes to the Python wrappers' own contracts.

Remote versions remain unchecked until downloaded. Cached incompatible versions
remain inspectable but cannot be selected for installation. Installation checks
the manifest before scheduling and again before publication; loading checks it
before executing native code or importing Python. Native DLL exports are still
validated separately: a compatible manifest does not override a bad binary.

The GUI displays compatible versions in green, incompatible versions in red,
and unchecked remote versions neutrally. The version chooser can download an
unchecked package and schedules it only after successful validation. Automatic
selection considers only verified compatible versions, ordered by package
version and then stable archive identifier.
