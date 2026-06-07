#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


HEADER_ROOT = Path(__file__).resolve().parents[1] / "c"
DEFAULT_OUTPUT = Path(__file__).resolve().with_name("slic3r_api_generated.py")

HEADER_ORDER = [
    "slic3r_def.h",
    "slic3r_utils.h",
    "slic3r_geometry.h",
    "slic3r_config_option_type.h",
    "slic3r_config_types.h",
    "slic3r_config_option.h",
    "slic3r_slicing_step.h",
    "slic3r_plugin_run_context.h",
    "slic3r_extrusions.h",
    "slic3r_extrusion_property.h",
    "slic3r_extrusion_polyline.h",
    "slic3r_extrusion_entity.h",
    "slic3r_data_tree.h",
    "slic3r_bridge_detector.h",
    "slic3r_clipper.h",
    "slic3r_config_def.h",
    "slic3r_orchestrator.h",
    "slic3r_volume.h",
]


TYPE_ALIASES = {
    "coord_t": "int64_t",
    "coordf_t": "double",
    "distf_t": "double",
    "distsqrf_t": "double",
    "coord_index_t": "int32_t",
    "lengthsqr_t": "uint64_t",
    "raw_surface_type": "uint16_t",
    "raw_extrusion_role": "int32_t",
    "raw_config_option_mode": "uint64_t",
    "RawConfigOptionFlags": "uint64_t",
    "slic3r_property_type": "uint32_t",
    "plugin_property_type": "uint32_t",
    "extrusion_property_type": "uint32_t",
    "extrusion_data_id": "uint32_t",
    "infill_pattern_runtime_id": "uint32_t",
    "expolygon_status": "int32_t",
    "c_extrusion_custom_gcode_kind": "int",
    "c_extrusion_special_command": "int",
    "clipper_end_type_t": "int",
    "clipper_join_type_t": "int",
    "clipper_operation_t": "int",
    "config_option_type": "int",
    "option_def_error_code": "int",
    "raw_config_option_type": "int",
    "raw_container_type": "int",
    "raw_extrusion_arc_orientation": "int",
    "raw_facet_painting_value": "int",
    "raw_gui_rule_action": "int",
    "raw_gui_rule_condition": "int",
    "raw_gui_type": "int",
    "raw_mesh_slicing_mode": "int",
    "raw_option_category": "int",
    "raw_option_level": "int",
    "raw_option_preset_type": "uint32_t",
    "raw_printer_technology": "int",
    "raw_volume_type": "int",
    "slicing_step_t": "uint16_t",
}

OPAQUE_TYPES = {
    "storage_handle",
    "orchestrator_handle",
    "print_config_def_handler",
    "plugin_host_context",
    "graph_data_handle",
    "config_option_handle",
    "config_option_vector_handle",
    "config_handle",
    "print_handle",
    "print_region_handle",
    "object_handle",
    "layer_handle",
    "layer_region_handle",
    "layer_island_handle",
    "layer_region_island_handle",
    "surface_handle",
    "surface_collection_handle",
    "extrusion_entity_handle",
    "clipper_shapes_handle",
    "polygon_handle",
    "polyline_handle",
    "multipoint_handle",
    "polygon_collection_handle",
    "polyline_collection_handle",
    "expolygon_handle",
    "expolygon_collection_handle",
    "volume_handle",
    "triangle_mesh_handle",
    "bridge_detector_vtable",
    "support_demand_handle",
    "slicing_layer_range_handle",
    "slicing_volume_region_handle",
}

CLASS_NAME_OVERRIDES = {
    "c_point": "CPoint",
    "c_vec3f": "CVec3f",
    "c_matrix4d": "CMatrix4d",
    "c_bounding_box": "CBoundingBox",
    "c_bounding_box3f": "CBoundingBox3f",
    "multipoint_view": "MultipointView",
    "multipoint_const_view": "MultipointConstView",
    "c_float_or_percent": "CFloatOrPercent",
    "const_strings_t": "ConstStrings",
    "key_value_string_pair_t": "KeyValueStringPair",
    "key_value_string_pair_array_t": "KeyValueStringPairArray",
    "option_enum_def_t": "OptionEnumDef",
    "raw_config_option_def": "RawConfigOptionDef",
    "c_layer_support_property": "CLayerSupportProperty",
    "c_layer_brim_property": "CLayerBrimProperty",
    "c_surface": "CSurface",
    "c_flow": "CFlow",
    "c_medial_axis_extrusion_params": "CMedialAxisExtrusionParams",
    "c_extrusion_segment": "CExtrusionSegment",
    "c_extrusion_flow": "CExtrusionFlow",
    "c_extrusion_property_attributes": "CExtrusionPropertyAttributes",
    "c_extrusion_property_speed": "CExtrusionPropertySpeed",
    "c_extrusion_property_modifier": "CExtrusionPropertyModifier",
    "c_extrusion_property_custom_gcode": "CExtrusionPropertyCustomGcode",
    "c_extrusion_property_special_command": "CExtrusionPropertySpecialCommand",
    "c_extrusion_property_overhang": "CExtrusionPropertyOverhang",
    "c_extrusion_property_z_offset": "CExtrusionPropertyZOffset",
    "c_extrusion_property_perimeter": "CExtrusionPropertyPerimeter",
    "c_extrusion_property_infill": "CExtrusionPropertyInfill",
    "bridge_detector_instance": "BridgeDetectorInstance",
    "bridge_detector_create_input": "BridgeDetectorCreateInput",
    "plugin_run_context": "PluginRunContext",
    "plugin_instance": "PluginInstance",
    "c_layer_config_range": "CLayerConfigRange",
    "c_triangle_indices": "CTriangleIndices",
    "c_mesh_slicing_params": "CMeshSlicingParams",
    "raw_gui_rule": "RawGuiRule",
    "raw_used_config_key": "RawUsedConfigKey",
}


@dataclass
class Field:
    name: str
    c_type: str
    array_size: int | None = None


@dataclass
class StructDef:
    c_name: str
    fields: list[Field]


@dataclass
class CallbackDef:
    c_name: str
    return_type: str
    args: list[str]


@dataclass
class FunctionDef:
    c_name: str
    return_type: str
    args: list[str]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    return text


def join_continuations(text: str) -> str:
    return text.replace("\\\n", " ")


def remove_preprocessor_lines(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def header_paths() -> list[Path]:
    ordered = [HEADER_ROOT / name for name in HEADER_ORDER]
    ordered += sorted((HEADER_ROOT / "steps").glob("*.h"))
    remaining = sorted(p for p in HEADER_ROOT.glob("*.h") if p not in ordered)
    return ordered + remaining


def class_name(c_name: str) -> str:
    if c_name in CLASS_NAME_OVERRIDES:
        return CLASS_NAME_OVERRIDES[c_name]
    parts = c_name.split("_")
    return "".join(part.capitalize() for part in parts)


def callback_name(c_name: str) -> str:
    name = c_name
    if name.endswith("_fn"):
        name = name[:-3]
    return name.upper()


def clean_type(c_type: str) -> str:
    c_type = " ".join(c_type.strip().split())
    c_type = c_type.replace("const ", "")
    c_type = c_type.replace(" *", "*").replace("*", " *")
    return " ".join(c_type.split())


def strip_casts(expr: str) -> str:
    return re.sub(
        r"\(\s*(?:uint\d+_t|int\d+_t|coord_t|distf_t|coordf_t|slic3r_property_type|plugin_property_type|extrusion_property_type|extrusion_data_id)\s*\)",
        "",
        expr,
    )


def sanitize_expr(expr: str) -> str:
    expr = strip_casts(expr)
    expr = expr.replace("UINT32_MAX", "0xFFFFFFFF")
    expr = expr.replace("UINT64_MAX", "0xFFFFFFFFFFFFFFFF")
    expr = re.sub(r"(?<=\d)[uUlLfF]+", "", expr)
    return expr.strip()


def try_eval(expr: str, values: dict[str, int | float | str]) -> int | float | str | None:
    expr = sanitize_expr(expr)
    if not expr:
        return None
    allowed = {name: value for name, value in values.items()}
    try:
        return eval(expr, {"__builtins__": {}}, allowed)
    except Exception:
        return None


def parse_constants(text: str) -> list[tuple[str, int | float | str]]:
    values: dict[str, int | float | str] = {}
    ordered: list[str] = []

    for match in re.finditer(r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)$", text, flags=re.M):
        name, expr = match.groups()
        if "(" in name:
            continue
        value = try_eval(expr, values)
        if value is None:
            continue
        if name not in values:
            ordered.append(name)
        values[name] = value

    enum_pattern = re.compile(
        r"(?:typedef\s+)?enum\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*:\s*[A-Za-z_][A-Za-z0-9_]*)?\s*"
        r"\{(?P<body>.*?)\}\s*(?:[A-Za-z_][A-Za-z0-9_]*)?\s*;",
        flags=re.S,
    )
    for match in enum_pattern.finditer(remove_preprocessor_lines(text)):
        current = -1
        for raw_item in match.group("body").split(","):
            item = raw_item.strip()
            if not item:
                continue
            if "=" in item:
                name, expr = [part.strip() for part in item.split("=", 1)]
                value = try_eval(expr, values)
                if value is None:
                    continue
                current = int(value)
            else:
                name = item
                current += 1
                value = current
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
                continue
            if name not in values:
                ordered.append(name)
            values[name] = value

    constexpr_pattern = re.compile(
        r"SLIC3R_CONSTEXPR_STATIC\s+(?:int|double|coord_t|coordf_t|distf_t)\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+);"
    )
    for name, expr in constexpr_pattern.findall(text):
        value = try_eval(expr, values)
        if value is None:
            continue
        if name not in values:
            ordered.append(name)
        values[name] = value

    return [(name, values[name]) for name in ordered]


def split_args(args: str) -> list[str]:
    args = args.strip()
    if not args or args == "void":
        return []
    out: list[str] = []
    cur = ""
    depth = 0
    for ch in args:
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
            continue
        cur += ch
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
    if cur.strip():
        out.append(cur.strip())
    return out


def arg_type(arg: str) -> str:
    arg = arg.strip()
    if "(*" in arg:
        match = re.match(r"(.+?)\(\s*\*\s*[A-Za-z_][A-Za-z0-9_]*\s*\)\s*\((.*)\)$", arg)
        if not match:
            raise ValueError(f"Cannot parse function pointer argument declaration: {arg}")
        return f"{match.group(1).strip()} (*)({match.group(2).strip()})"
    if re.match(r"^(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*(?:\s*\*)+$", arg):
        return arg
    match = re.match(r"(.+?)([A-Za-z_][A-Za-z0-9_]*)$", arg)
    if not match:
        raise ValueError(f"Cannot parse argument declaration: {arg}")
    return match.group(1).strip()


def parse_callbacks(text: str) -> list[CallbackDef]:
    callbacks: list[CallbackDef] = []
    pattern = re.compile(
        r"typedef\s+(?P<ret>[^;{}]+?)\s*\(\s*\*\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)"
        r"\s*\((?P<args>[^;{}]*?)\)\s*;",
        flags=re.S,
    )
    for match in pattern.finditer(remove_preprocessor_lines(text)):
        args = [arg_type(arg) for arg in split_args(match.group("args"))]
        callbacks.append(CallbackDef(match.group("name"), match.group("ret").strip(), args))
    return callbacks


def parse_field(decl: str) -> Field | None:
    decl = decl.strip()
    if not decl:
        return None
    if "(*" in decl:
        return None

    array_size: int | None = None
    array_match = re.search(r"\[([0-9]+)\]\s*$", decl)
    if array_match:
        array_size = int(array_match.group(1))
        decl = decl[: array_match.start()].strip()

    match = re.match(r"(.+?)([A-Za-z_][A-Za-z0-9_]*)$", decl)
    if not match:
        return None
    return Field(match.group(2), match.group(1).strip(), array_size)


def parse_structs(text: str) -> list[StructDef]:
    structs: list[StructDef] = []
    parsed_names: set[str] = set()
    body_text = remove_preprocessor_lines(text)

    typedef_pattern = re.compile(
        r"typedef\s+struct(?:\s+[A-Za-z_][A-Za-z0-9_]*)?\s*\{(?P<body>.*?)\}\s*"
        r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*;",
        flags=re.S,
    )
    for match in typedef_pattern.finditer(body_text):
        name = match.group("name")
        if name.endswith("_vtable") or name == "plugin_vtable":
            continue
        fields = [field for field in (parse_field(part) for part in match.group("body").split(";")) if field]
        structs.append(StructDef(name, fields))
        parsed_names.add(name)

    named_pattern = re.compile(
        r"^\s*struct\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\{(?P<body>.*?)^\s*\}\s*;",
        flags=re.S | re.M,
    )
    for match in named_pattern.finditer(body_text):
        name = match.group("name")
        if name in parsed_names or name.endswith("_vtable") or name == "plugin_vtable":
            continue
        fields = [field for field in (parse_field(part) for part in match.group("body").split(";")) if field]
        structs.append(StructDef(name, fields))
        parsed_names.add(name)

    return structs


def parse_functions(text: str) -> list[FunctionDef]:
    functions: list[FunctionDef] = []
    body_text = remove_preprocessor_lines(text)
    pattern = re.compile(r"SLIC3R_HOST_API\s+([^;]+);", flags=re.S)
    for match in pattern.finditer(body_text):
        proto = " ".join(match.group(1).split())
        parsed = re.match(r"(.+?)\s*([*]?)\s*([A-Za-z_][A-Za-z0-9_]*)\((.*)\)$", proto)
        if not parsed:
            raise ValueError(f"Cannot parse exported function: {proto}")
        ret, star, name, args = parsed.groups()
        return_type = (ret.strip() + (" *" if star else "")).strip()
        functions.append(FunctionDef(name, return_type, [arg_type(arg) for arg in split_args(args)]))
    return functions


def c_base_type(c_type: str) -> str:
    c_type = clean_type(c_type)
    while c_type in TYPE_ALIASES:
        c_type = TYPE_ALIASES[c_type]
    return c_type


def ctype_expr(c_type: str, class_names: set[str], callback_names: dict[str, str], *, for_return: bool = False) -> str:
    c_type = clean_type(c_type)
    function_pointer = re.match(r"(.+?)\s+\(\s*\*\s*\)\s*\((.*)\)$", c_type)
    if function_pointer:
        return_expr = ctype_expr(function_pointer.group(1), class_names, callback_names, for_return=True)
        arg_exprs = [ctype_expr(arg_type(arg), class_names, callback_names) for arg in split_args(function_pointer.group(2))]
        return f"ctypes.CFUNCTYPE({', '.join([return_expr] + arg_exprs)})"

    pointer_depth = c_type.count("*")
    base = c_type.replace("*", "").strip()

    if pointer_depth >= 2:
        return "ctypes.POINTER(ctypes.c_void_p)"
    if pointer_depth == 1:
        if base == "char":
            return "ctypes.c_char_p"
        if base == "void" or base in OPAQUE_TYPES:
            return "ctypes.c_void_p"
        if base in class_names:
            return f"ctypes.POINTER({class_name(base)})"
        resolved = c_base_type(base)
        if resolved in PRIMITIVE_CTYPES:
            return f"ctypes.POINTER({PRIMITIVE_CTYPES[resolved]})"
        return "ctypes.c_void_p"

    if base in callback_names:
        return callback_names[base]
    if base in class_names:
        return class_name(base)
    resolved = c_base_type(base)
    if resolved in PRIMITIVE_CTYPES:
        return PRIMITIVE_CTYPES[resolved]
    if resolved in OPAQUE_TYPES:
        return "ctypes.c_void_p"
    raise ValueError(f"Cannot map C type to ctypes: {c_type}")


PRIMITIVE_CTYPES = {
    "void": "None",
    "int": "ctypes.c_int",
    "int32_t": "ctypes.c_int32",
    "uint8_t": "ctypes.c_uint8",
    "uint16_t": "ctypes.c_uint16",
    "uint32_t": "ctypes.c_uint32",
    "uint64_t": "ctypes.c_uint64",
    "int16_t": "ctypes.c_int16",
    "int64_t": "ctypes.c_int64",
    "double": "ctypes.c_double",
    "float": "ctypes.c_float",
    "size_t": "ctypes.c_size_t",
}


def type_dependencies(field: Field, class_names: set[str], callback_names: dict[str, str]) -> set[str]:
    c_type = clean_type(field.c_type)
    if c_type in callback_names:
        return set()
    if "*" in c_type:
        return set()
    base = c_base_type(c_type)
    return {base} if base in class_names else set()


def order_structs(structs: list[StructDef], callback_names: dict[str, str]) -> list[StructDef]:
    by_name = {struct.c_name: struct for struct in structs}
    class_names = set(by_name)
    done: set[str] = set()
    ordered: list[StructDef] = []

    while len(done) < len(structs):
        progressed = False
        for struct in structs:
            if struct.c_name in done:
                continue
            deps: set[str] = set()
            for field in struct.fields:
                deps.update(type_dependencies(field, class_names, callback_names))
            deps.discard(struct.c_name)
            if deps <= done:
                ordered.append(struct)
                done.add(struct.c_name)
                progressed = True
        if not progressed:
            pending = sorted(set(by_name) - done)
            raise ValueError(f"Cannot order generated ctypes structures, dependency cycle: {pending}")

    return ordered


def generate() -> str:
    raw_text = "\n".join(path.read_text(encoding="utf-8") for path in header_paths() if path.exists())
    text = join_continuations(strip_comments(raw_text))
    constants = parse_constants(text)
    callbacks = parse_callbacks(text)
    structs = parse_structs(text)
    functions = parse_functions(text)

    callback_names = {callback.c_name: callback_name(callback.c_name) for callback in callbacks}
    class_names = {struct.c_name for struct in structs}

    lines: list[str] = [
        "#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill",
        "#/|/",
        "#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher",
        "#/|/",
        "",
        '"""Generated ctypes bindings for the public SuperSlicer plugin C ABI.',
        "",
        "Do not edit this file by hand. Run generate_slic3r_api.py instead.",
        '"""',
        "",
        "from __future__ import annotations",
        "",
        "import ctypes",
        "",
        "",
    ]

    emitted_constants: set[str] = set()
    for name, value in constants:
        if name in emitted_constants:
            continue
        emitted_constants.add(name)
        lines.append(f"{name} = {repr(value)}")
    lines.append("")

    for struct in structs:
        lines.append("")
        lines.append(f"class {class_name(struct.c_name)}(ctypes.Structure):")
        lines.append("    pass")

    for callback in callbacks:
        return_expr = ctype_expr(callback.return_type, class_names, callback_names, for_return=True)
        arg_exprs = [ctype_expr(arg, class_names, callback_names) for arg in callback.args]
        lines.append(f"{callback_name(callback.c_name)} = ctypes.CFUNCTYPE({', '.join([return_expr] + arg_exprs)})")
    lines.append("")

    for struct in order_structs(structs, callback_names):
        emit_struct_fields(lines, struct, class_names, callback_names)

    lines.extend(
        [
            "def _bind_c_function(host, name, restype, argtypes) -> None:",
            "    try:",
            "        fn = getattr(host, name)",
            "    except AttributeError:",
            "        return",
            "    fn.restype = restype",
            "    fn.argtypes = argtypes",
            "",
            "",
            "C_FUNCTION_SIGNATURES = [",
        ]
    )

    for function in functions:
        return_expr = ctype_expr(function.return_type, class_names, callback_names, for_return=True)
        arg_exprs = [ctype_expr(arg, class_names, callback_names) for arg in function.args]
        lines.append(f'    ("{function.c_name}", {return_expr}, [{", ".join(arg_exprs)}]),')
    lines.append("]")
    lines.append("")
    lines.append("")
    lines.append("__all__ = [name for name in globals() if not name.startswith('_')]")
    lines.append("")
    return "\n".join(lines)


def emit_struct_fields(lines: list[str], struct: StructDef, class_names: set[str], callback_names: dict[str, str]) -> None:
    if not struct.fields:
        return
    lines.append(f"{class_name(struct.c_name)}._fields_ = [")
    for field in struct.fields:
        field_type = ctype_expr(field.c_type, class_names, callback_names)
        if field.array_size is not None:
            field_type = f"{field_type} * {field.array_size}"
        lines.append(f'    ("{field.name}", {field_type}),')
    lines.append("]")
    lines.append("")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate Python ctypes bindings for the SuperSlicer C plugin ABI.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="generated Python output path")
    parser.add_argument("--check", action="store_true", help="fail if the output file is not up to date")
    args = parser.parse_args()

    generated = generate()
    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
        if current != generated:
            print(f"{args.output} is not up to date. Regenerate it with generate_slic3r_api.py.")
            return 1
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
