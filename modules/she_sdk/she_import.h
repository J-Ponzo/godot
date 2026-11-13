/**************************************************************************/
/*  she_import.h                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class Node;

namespace she_sdk {

// Internal marker interpretation (chantier `sdk`, batch 2A2), consumed by
// the registered `SHEMarkerInterpreter` engine class (she_import_interpreter.h)
// which the editor scene importer dispatches to through ClassDB - the editor
// never links against module symbols (Godot's canonical decoupling).
//
// GATEWAY (acté 2026-11-13): the whole interpretation is gated on the
// scene flag `she_scene`, transported Blender -> glTF -> Godot by the
// additive `GLTFDocument` scene-extras channel: the Blender exporter writes
// the scene's custom properties to the glTF `scenes[].extras` dictionary,
// GLTFDocument::_parse_scenes keeps them in GLTFState::scene_extras and
// attach_scene_extras metadata `gltf_scene_extras` on the generated scene
// root. scene_is_she() reads the flag there. Without it nothing below runs:
// every channel (including the suffixed-name chain) stays purely vanilla
// for every node, `she_*` properties are inert data - and markers present
// in an unflagged scene are a configuration error, reported fail-loudly
// (same matrix at every layer: runner at export, module at import).
//
// Blender custom properties `she_*` travel to the engine as glTF node extras,
// which the glTF importer attaches to every generated node as the "extras"
// metadata dictionary. In a flagged scene, the interpretation happens at
// three delegation points of ResourceImporterScene::import_scene:
//
// 1. Chain resolution (from _pre_fix_node): a node carrying at least one
//    `she_*` property is excluded from the suffixed-name chain (accidental
//    native suffixes on a marked node would silently destroy what the
//    markers describe). resolve_chain_name() reports the name the chain must see:
//    "" when the plan executes through node settings and/or the module-side
//    visual drop, or "<name>-colonly"-style virtual suffixes for the native
//    "drop single" states, so the vanilla branch chain itself executes them
//    (renames included, hence dump parity with a suffixed upstream object).
//
// 2. The import plan (after the chain, before _post_fix_node):
//    build_import_plan() validates the marker matrix (fail-loudly, one
//    message per bad object) and computes the node settings to inject into
//    the `_subresources` "nodes" channel for the mirror axes:
//    she_collision -> generate/physics + physics/body_type (+ physics/shape_type),
//    she_occluder -> generate/occluder, she_navmesh -> generate/navmesh,
//    keyed exactly like the importer's node-settings lookup. The importer's
//    vanilla node-settings code then executes them unchanged.
//
// 3. The module-side visual drop (right after _post_fix_node, once the
//    occluder arrays are baked): states with no native single counterpart
//    (e.g. the deterministic wall "collider + occluder + drop") delete the
//    mesh node here, keeping its derivatives (bodies, navigation regions)
//    at its place with compensated transforms. Nodes already consumed by the
//    chain ("colonly"/"occonly" singles) are no longer mesh instances by then
//    and are naturally left alone.

bool scene_is_she(const Node *p_scene_root);
bool has_she_markers(const Node *p_node);
bool resolve_chain_name(const Node *p_scene_root, const Node *p_node, String &r_chain_name);
void build_import_plan(Node *p_scene_root, Dictionary &r_settings_by_key, Vector<String> &r_errors);
void apply_visual_drops(Node *p_scene_root);

} // namespace she_sdk
