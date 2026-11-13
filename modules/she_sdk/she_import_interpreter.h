/**************************************************************************/
/*  she_import_interpreter.h                                              */
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

#include "core/object/ref_counted.h"
#include "scene/main/node.h"

// Engine class exposing the SHE import-marker interpretation (chantier `sdk`,
// batch 2A2) to the editor scene importer, through ClassDB dispatch - the
// editor never links against module symbols (Godot's canonical decoupling,
// see the gridmap/gltf module patterns). ResourceImporterScene's delegation
// points instantiate this class once ("SHEMarkerInterpreter") and call its
// methods at import time. The methods are engine-facing plumbing only; the
// marker doctrine lives in `she_import.h` and the chantier docs.
class SHEMarkerInterpreter : public RefCounted {
	GDCLASS(SHEMarkerInterpreter, RefCounted);

public:
	// Gateway check (acté 2026-11-13): does the scene root carry the
	// `she_scene` flag (glTF `scenes[].extras` -> "gltf_scene_extras"
	// metadata, transported additively by GLTFDocument)? Without it every
	// method below is a no-op / pure-vanilla pass-through.
	bool scene_is_she(Node *p_scene_root) const;

	// Quick check used per node by the suffix chain: does this node carry at
	// least one `she_*` property (glTF extras metadata)?
	bool has_she_markers(Node *p_node) const;

	// The name the suffixed-name chain must evaluate for a node of a flagged
	// scene: "" (unmatchable: the marker plan owns the node's roles), a
	// virtual suffix name ("wall-colonly"-style) whose native branch the
	// vanilla chain executes (renames included), or the name unchanged
	// (unflagged scene or unmarked node: purely vanilla chain).
	String chain_name_for(Node *p_scene_root, Node *p_node, const String &p_name) const;

	// Marker matrix validation + the node settings to inject into the
	// `_subresources` "nodes" channel, keyed exactly like the importer's
	// node-settings lookup. Returns { "settings": Dictionary,
	// "errors": PackedStringArray }; the importer merges "settings" itself.
	Dictionary marker_import_plan(Node *p_scene_root) const;

	// Module-side visual drop, executed after the occluder arrays are baked
	// (see she_import.h point 3).
	void apply_visual_drops(Node *p_scene_root) const;

protected:
	static void _bind_methods();
};
