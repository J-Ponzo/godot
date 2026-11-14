/**************************************************************************/
/*  she_dg.h                                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to       */
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

class Node;

namespace she_sdk {

// DG post-import (chantier `sdk`, batch 2A3 J3), reached through the
// registered SHEMarkerInterpreter engine class exactly like the marker
// interpretation (same ClassDB decoupling). Runs on a `she_scene`-flagged
// import only, and is a silent no-op without a `dg/bake.json` next to the
// imported source (an unbaked work-in-progress level). Contract:
//
// - the bake descriptor `dg/bake.json` (schema `she-bake/1`, produced by
//   the Blender side - she-editor or the harness bake) is the authority:
//   camera names, clip and z_to remap semantics, and the flat map file
//   names, all living in the sibling `dg/` directory of the imported
//   scene source (`levels/<id>/dg/` in the project doctrine);
// - the `.exr.import` of every map of the bake is patched with the
//   float-exact options (route A, acté at the 2A3 cadrage: the default
//   EXR import is already float-exact - Lossless remaps float formats to
//   VRAM Uncompressed - the patch neutralizes the `detect_3d` flip and
//   pins the lossless mode), then the native "a modified `.import` forces
//   the reimport" mechanism applies them (proven by the 2A2 settings
//   family);
// - one `.tres` DGBuffer per baked camera is generated from the maps
//   through the standard texture import cache, named by camera name
//   (`dg/<camera_name>.tres` - machine naming owned by the tools);
// - cameras of the imported scene are bound by name (the glTF node name
//   carries the Blender camera name), with the clip coherence validated
//   fail-loudly (an exact comparison: both sides round the same Blender
//   double through float32 - a diverging near/far after a bake means the
//   maps' depth remap no longer matches the camera and the G-buffer init
//   would silently misdecode: no bind happens, the error is printed).
//   A baked camera without a matching imported node is normal (e.g. a
//   `she_noimp` camera) and stays silent; the z_to remap is validated
//   against the init semantics ([0, 1] shell remap) before generating.
//
// All diagnostics carry the `[SHE-DG]` prefix (import-time counterpart of
// the runtime `[SHE-DUMP]` markers, grepped by the sdk harness audits).

void apply_dg_post_import(Node *p_scene_root, const String &p_source_file);

} // namespace she_sdk
