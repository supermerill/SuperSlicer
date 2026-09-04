# Joint Activation Groups

An activation group declares plugin IDs that must be enabled or disabled
together. It does not impose execution order and does not replace technical
dependencies or exclusive provider selection.

Declare groups during plugin registration, before or after registering their
members. The host checks member existence after all plugins have loaded.

```cpp
slic3r_api::OrchestratorView(orch).register_activation_group("dense_infill", {
    "dense_infill.surface_marker",
    "dense_infill.recipe_modifier",
    "dense_infill.post_infill_order"
});
```

Python exposes the same operation as `api.register_activation_group(id, members)`.
The C entry point is `orchestrator_register_activation_group(orch, id, members)`;
it copies the strings and returns 1 on success, or 0 with a logged diagnostic.
The wrappers raise an exception on rejection. This requires version 1.1 of
`slic3r_orchestrator.h`, without changing the plugin vtable.

Each declaration needs a nonempty group ID and at least two distinct nonempty
member IDs. Repeating an ID within a call is invalid. Repeated declarations of
the same group accumulate their union; they never remove a member. Groups may
overlap: `{A, B}` and `{B, C}` make all three plugins jointly activated.

The managers propose the complete transitive change, including dependencies.
One confirmation lists additional solidarity and dependency changes. Refusing
activation leaves the requested solidarity component disabled; refusing
deactivation leaves the selection unchanged.

At startup, incomplete or partially selected components are blocked, followed
by dependent consumers. The configuration is not rewritten and no missing
activation is silently supplied. Incomplete declarations appear in Problems
even when inactive; startup notifications are emitted only for blocked requests.
