# Using Plugin Properties

> API snapshot commit: `2273b753a20b520d8ca2b275299e4003e0f680ab`
>
> Plugin ABI version: `50`

The commit above identifies the source-tree state against which this guide and
its examples were checked. It is not the commit that adds this document: a
commit cannot contain its own final hash. Update both the snapshot hash and the
ABI value whenever this guide is revised for a later plugin API.

Plugin properties are small typed metadata records attached to objects already
owned by the slicer. They let one plugin annotate an object and a later plugin
consume that annotation without adding a field to the native object or coupling
the two plugins to a host implementation class.

This guide uses the C++ API as the primary interface. The final section maps the
same concepts to the C ABI and Python views.

## The Three Parts Of A Property

Every property has:

1. A **payload type**, which describes the bytes stored on an object.
2. A **numeric property type**, which identifies what those bytes mean.
3. A **typed key**, `PluginPropertyKey<Payload>`, which keeps the payload type,
   numeric type and registering orchestrator together.

Property payloads are copied as raw bytes, and newly created payloads are
zero-initialized. They must be trivially copyable and must not own memory. Do
not put `std::string`, `std::vector`, smart pointers, virtual methods or other
resource-owning C++ objects inside a payload.

There are two kinds of key:

- **Built-in keys** have a stable ID published by the host. Their C++ payloads
  expose a ready-to-use static key such as `EPropertySpeed::key`.
- **Dynamic keys** are registered by a plugin under a stable namespaced name.
  Their compact numeric ID belongs to one orchestrator and may differ in another
  run. Keep the key in the plugin instance and never serialize `key.type()`.

The same `PluginPropertyKey<Payload>` can address a generic data-tree property
or an extrusion property. The owner passed to the key selects the storage.

## Choosing The Property Owner

| Owner | C++ access | Lookup behavior |
| --- | --- | --- |
| `Surface`, `Layer`, `LayerRegion`, `LayerIsland`, `LayerRegionIsland`, `Object` | `view.properties()` | Direct metadata on that object; no inheritance |
| `PrintingPlan`, `PrintingGroup`, `PrintingLayerGroup`, `PrintingToolGroup` | `scope.properties()` | Direct metadata on that final-plan scope; no inheritance |
| `ExtrusionEntity` and mutable extrusion views | `entity.get(key)` | Direct property on that node only |
| An extrusion tree being visited | `current_property(key)` | Effective property inherited from the nearest node toward the root |

Use the narrowest owner whose lifetime matches the information. For example, a
classification of one surface belongs on that `Surface`; a value applying to
all descendants of one extrusion subtree belongs on its parent extrusion node;
and a duration for a final ordered layer belongs on its `PrintingLayerGroup`.

## Using A Built-in Property

Built-in data-tree payloads are declared in
[`DataTreeProperties.hpp`](../../src/libslic3r/Api/plugin/cpp/properties/DataTreeProperties.hpp).
Built-in extrusion payloads are declared in
[`ExtrusionProperties.hpp`](../../src/libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp).

The static `key` is the preferred API for every operation:

```cpp
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"

using namespace slic3r_api;

void annotate_support_layer(const Layer &layer)
{
    PluginProperties properties = layer.properties();

    LayerSupportProperty &support =
        properties.get_or_add(LayerSupportProperty::key);
    support.interface_id = 2;

    if (const LayerSupportProperty *stored =
            properties.get(LayerSupportProperty::key)) {
        use_interface_id(stored->interface_id);
    }

    properties.remove(LayerSupportProperty::key);
}
```

The key-first syntax is equivalent and is useful when a helper already owns the
key:

```cpp
const LayerSupportProperty *support =
    LayerSupportProperty::key.get(layer.properties());
```

For an extrusion, pass the same kind of key directly to the entity view:

```cpp
const EPropertySpeed *speed = entity.get(EPropertySpeed::key);

EPropertySpeed &editable_speed =
    mutable_entity.get_or_add(EPropertySpeed::key);
editable_speed.speed(35.f).acceleration(800.f);
```

`get()` and `get_mutable()` return `nullptr` when the direct property is absent.
`get_or_add()` returns the existing payload or creates a zero-initialized one.
`remove()` returns whether a property was actually removed.

## Defining A Dynamic Property

A dynamic payload is an ordinary trivial struct. Register its stable name in
every plugin instance that produces or consumes it, and retain the returned key
as a member:

```cpp
#include <cstdint>
#include <type_traits>

#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"

struct SurfaceQualityHint
{
    uint32_t source_id;
    float score;
};

static_assert(std::is_trivially_copyable<SurfaceQualityHint>::value,
              "A plugin property payload must be trivially copyable.");

class QualityPluginState
{
public:
    explicit QualityPluginState(orchestrator_handle *orchestrator) :
        m_quality_hint(
            slic3r_api::PluginPropertyKey<SurfaceQualityHint>::register_dynamic(
                orchestrator,
                "com.example.quality.surface_hint"))
    {
    }

private:
    slic3r_api::PluginPropertyKey<SurfaceQualityHint> m_quality_hint;
};
```

The name is the contract between plugins. Registering the same name, payload
size and alignment on one orchestrator returns the same runtime ID. Registering
the same name with an incompatible layout is rejected.

Once registered, built-in and dynamic properties use exactly the same syntax:

```cpp
SurfaceQualityHint &hint =
    surface.mutable_properties().get_or_add(m_quality_hint);
hint.source_id = source_id;
hint.score = score;

const SurfaceQualityHint *stored = m_quality_hint.get(surface.properties());
```

Default member initializers and constructors are not executed when the host
creates a payload. Initialize every field whose valid default is not all-zero
after detecting that the property is new.

## Direct And Inherited Extrusion Properties

`entity.get(key)` reads only the property physically stored on that entity. It
does not inspect parents. This makes direct lookup predictable and inexpensive.

During a tree traversal, use `current_property(key)` to resolve the effective
value. It searches from the current node toward the root and returns the nearest
definition:

```cpp
#include "libslic3r/Api/plugin/cpp/ExtrusionTreeVisitors.hpp"

class SpeedReader : public slic3r_api::ExtrusionTreeConstVisitor<>
{
protected:
    void visit_leaf(slic3r_api::ExtrusionEntity leaf) override
    {
        const slic3r_api::EPropertySpeed *speed =
            current_property(slic3r_api::EPropertySpeed::key);
        if (speed != nullptr)
            consume_speed(leaf, speed->speed_mm_per_s);
    }
};
```

A child property overrides a parent property of the same type for that subtree.
The returned pointer is borrowed from whichever entity owns the winning
property. Do not retain it after mutating the tree.

## Storing Variable-size Extrusion Data

Property payloads stay small and fixed-size. An extrusion property that needs a
string or larger binary value stores an `extrusion_data_id` in its payload and
keeps the bytes in the extrusion entity's auxiliary storage.

Use `store_property_string()` or `store_property_value()` when the data is owned
by a field in a property. The host then replaces the previous buffer safely and
releases it when the property is removed:

```cpp
struct LabelProperty
{
    extrusion_data_id label_id;
    uint32_t category;
};

const bool already_present = mutable_entity.has(m_label_key);
LabelProperty &label = mutable_entity.get_or_add(m_label_key);
if (!already_present)
    label.label_id = EXTRUSION_DATA_ID_INVALID;

if (mutable_entity.store_property_string(
        m_label_key.type(), &label.label_id, text) ==
    EXTRUSION_DATA_ID_INVALID) {
    throw std::runtime_error("The extrusion label could not be stored.");
}
```

The initialization to `EXTRUSION_DATA_ID_INVALID` matters because a newly
created payload is zeroed, while the invalid data ID is not zero.

Use `store_string()` or `store_value()` only for standalone data whose lifetime
is managed separately with `free_data()`. Auxiliary data belongs to the same
entity that stores it. An inherited payload pointer alone does not identify the
parent entity that owns its referenced data.

Generic data-tree and PrintingPlan properties do not have property-owned
variable buffers. Use `PrintRecords` for variable, print-wide `DynamicConfig`
data instead of putting an owning pointer in a property.

## Lifetime, Mutation And Threading

- `PluginPropertyKey` is a small non-owning value. A dynamic key borrows its
  orchestrator and must not outlive that plugin runtime.
- Pointers and references returned by property access are borrowed. Treat them
  as short-lived and reacquire them after modifying, moving or replacing the
  owner.
- A const data-tree view may expose mutable plugin metadata through
  `properties()`. This does not make the object's geometry or native fields
  mutable.
- Property containers are not synchronized. Parallel workers may modify
  distinct owners. Concurrent mutation of the same owner requires plugin-side
  synchronization.
- Copying or moving a host object follows that object's documented property
  behavior. Do not use a property pointer as an ownership or identity token.

## Property, Config, PrintRecord Or Auxiliary Data?

| Need | Preferred mechanism |
| --- | --- |
| Small fixed metadata attached to one data-tree object or plan scope | `PluginProperties` |
| Small fixed process metadata attached to an extrusion subtree | Extrusion property |
| Text or binary data owned by an extrusion property | `extrusion_data_id` with `store_property_*()` |
| Typed option values, serialization or script arguments | `Config` / `DynamicConfig` |
| Variable print-wide tables or shared records | `PrintRecords` |

Avoid using a property as a general object store. Its strength is a compact,
copyable contract that follows the object carrying it.

## C And Python Equivalents

| Operation | C++ | C | Python |
| --- | --- | --- | --- |
| Register a dynamic property | `PluginPropertyKey<T>::register_dynamic()` | `orchestrator_register_property()` | Custom extrusion helper: `api.register_extrusion_property_type()` |
| Read data-tree metadata | `properties.get(key)` | `plugin_property_data()` | `properties.get(PayloadClass)` |
| Create data-tree metadata | `properties.get_or_add(key)` | `plugin_property_get_or_add_data_mutable()` | `properties.get_or_add(PayloadClass)` |
| Read a direct extrusion property | `entity.get(key)` | `extrusion_property_data()` | `entity.property(PayloadClass)` |
| Create an extrusion property | `entity.get_or_add(key)` | `extrusion_property_get_or_add_data_mutable()` | `entity.property(PayloadClass)` on a mutable view |
| Remove a property | `owner.remove(key)` | `plugin_property_remove()` / `extrusion_property_remove()` | `remove()` / `remove_property()` |

Python identifies a property through the numeric `property_type` attached to a
`ctypes` payload class; it does not currently expose the C++
`PluginPropertyKey` object. Read-only extrusion views return payload copies,
while mutable views return live `ctypes` views over host memory. The current
high-level custom registration helper targets extrusion properties; generic
data-tree registration may require the lower-level host call.

## Review Checklist

Before publishing a property contract, verify that:

- the payload is trivially copyable and has no owning members;
- its dynamic name is stable and namespaced;
- its key is stored per orchestrator and its numeric ID is never serialized;
- every non-zero default or sentinel is initialized after creation;
- direct versus inherited lookup is intentional;
- borrowed pointers do not survive owner mutations;
- parallel writers cannot reach the same owner unsynchronized;
- variable data uses the appropriate auxiliary storage instead of an in-payload
  pointer.
