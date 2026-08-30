///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "MarkFirstLoop.hpp"

#include <cassert>
#include <cstdint>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"

namespace slic3r_api { namespace Perimeter { namespace MarkFirstLoopPlugin {

namespace {

const char *k_mark_first_loop_id = "perimeter.module.mark_first_loop";
const char *k_no_dependencies[] = { nullptr };
const uint16_t k_loop_flag = uint16_t(C_EXTRUSION_PERIMETER_FLAG_LOOP);
const uint16_t k_first_loop_flag = uint16_t(C_EXTRUSION_PERIMETER_FLAG_FIRST_LOOP);

/*
Mark-first-loop module
======================

The perimeter generator builds a tree where every child is farther inside the
island than its parent. That tree carries the exact answer to "is there another
perimeter inside this one?", so this module waits until end(), when the tree is
complete, and walks it from children to parents.

Only real perimeter loops are tagged. Gap fill and thin-wall extrusions can
live in the perimeter tree, but they are not perimeter shells from the point of
view of shell ordering, so they must not receive FIRST_LOOP.
*/

raw_extrusion_role effective_role(MutableExtrusionEntity entity)
{
    // EPropertyAttributes may be stored on the loop itself or on the printable
    // child path. For this module the role is only used as a safety filter, so
    // looking at the first descendant role is sufficient.
    if (const EPropertyAttributes *attributes = entity.get(EPropertyAttributes::key))
        return raw_extrusion_role(attributes->extrusion_role());

    for (uint32_t idx = 0; idx < entity.child_count(); ++idx) {
        const raw_extrusion_role child_role = effective_role(entity.child_mutable(idx));
        if (child_role != RAW_EXTRUSION_ROLE_NONE)
            return child_role;
    }

    return RAW_EXTRUSION_ROLE_NONE;
}

bool role_is_gap_fill_or_thin_wall(raw_extrusion_role role)
{
    return role == RAW_EXTRUSION_ROLE_GAP_FILL || (role & RAW_EXTRUSION_ROLE_THIN) != 0;
}

bool entity_has_taggable_loop(MutableExtrusionEntity entity)
{
    const EPropertyPerimeter *perimeter = entity.get(EPropertyPerimeter::key);
    if (perimeter == nullptr || (perimeter->perimeter_flags() & k_loop_flag) == 0)
        return false;

    const raw_extrusion_role role = effective_role(entity);
    return !role_is_gap_fill_or_thin_wall(role);
}

bool entity_contains_taggable_loop(MutableExtrusionEntity entity)
{
    if (entity_has_taggable_loop(entity))
        return true;

    for (uint32_t idx = 0; idx < entity.child_count(); ++idx)
        if (entity_contains_taggable_loop(entity.child_mutable(idx)))
            return true;

    return false;
}

bool mark_first_loop_in_entity(MutableExtrusionEntity entity)
{
    // Entity subtrees produced by generators mirror the perimeter-node tree:
    // loop wrappers may contain one printable child, and collection nodes may
    // contain several loops. Children are examined first so a parent loop is
    // tagged only when no deeper loop was found.
    bool child_has_taggable_loop = false;
    for (uint32_t idx = 0; idx < entity.child_count(); ++idx)
        child_has_taggable_loop = mark_first_loop_in_entity(entity.child_mutable(idx)) || child_has_taggable_loop;

    if (!entity_has_taggable_loop(entity))
        return child_has_taggable_loop;

    if (!child_has_taggable_loop) {
        EPropertyPerimeter &perimeter = entity.get_or_add(EPropertyPerimeter::key);
        perimeter.perimeter_flags(uint16_t(perimeter.perimeter_flags() | k_first_loop_flag));
    }

    return true;
}

bool mark_first_loop_in_node(perimeter_node *node)
{
    assert(node != nullptr);
    if (node == nullptr)
        return false;

    // Work bottom-up through perimeter nodes. Empty child nodes return false,
    // which lets the nearest parent with real perimeter extrusion become the
    // innermost loop for that branch.
    bool child_has_taggable_loop = false;
    for (uint32_t idx = 0; idx < node->child_count; ++idx)
        child_has_taggable_loop = mark_first_loop_in_node(node->children[idx]) || child_has_taggable_loop;

    if (node->extrusions == nullptr)
        return child_has_taggable_loop;

    MutableExtrusionEntity extrusions(node->extrusions);
    if (child_has_taggable_loop)
        return entity_contains_taggable_loop(extrusions) || child_has_taggable_loop;

    return mark_first_loop_in_entity(extrusions);
}

void module_end(void *, void *, perimeter_generation_context *context)
{
    assert(context != nullptr);
    assert(context == nullptr || context->root != nullptr);
    if (context == nullptr || context->root == nullptr)
        return;

    mark_first_loop_in_node(context->root);
}

const perimeter_generation_module_vtable &module_vtable()
{
    static const perimeter_generation_module_vtable vt = {
        nullptr,
        nullptr,
        nullptr,
        &module_end
    };
    return vt;
}

} // namespace

MarkFirstLoop &MarkFirstLoop::instance(orchestrator_handle *orch)
{
    static MarkFirstLoop s_instance(orch);
    return s_instance;
}

const char *MarkFirstLoop::id_impl() const noexcept
{
    return k_mark_first_loop_id;
}

slicing_step_t MarkFirstLoop::step_impl() const noexcept
{
    return PERIMETER_GENERATION_MODULE;
}

const char *const *MarkFirstLoop::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t MarkFirstLoop::priority_impl() const noexcept
{
    return 100;
}

const char *MarkFirstLoop::progress_message_format_impl() const noexcept
{
    return "Marking first perimeter loops: %u / %u";
}

void MarkFirstLoop::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_perimeter_generation_module *ctx = plugin_ctx_as_perimeter_generation_module(run_ctx);
    if (ctx == nullptr)
        return;

    ctx->module.ctx = const_cast<MarkFirstLoop *>(this);
    ctx->module.vt = &module_vtable();
}

void register_mark_first_loop_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, MarkFirstLoop::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::MarkFirstLoopPlugin
