/**************************************************************************/
/*  she_import_interpreter.cpp                                            */
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

#include "she_import_interpreter.h"

#include "core/object/class_db.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

#include "she_import.h"

bool SHEMarkerInterpreter::scene_is_she(Node *p_scene_root) const {
	return she_sdk::scene_is_she(p_scene_root);
}

bool SHEMarkerInterpreter::has_she_markers(Node *p_node) const {
	return she_sdk::has_she_markers(p_node);
}

String SHEMarkerInterpreter::chain_name_for(Node *p_scene_root, Node *p_node, const String &p_name) const {
	String chain_name = p_name;
	she_sdk::resolve_chain_name(p_scene_root, p_node, chain_name);
	// "" means the marker plan owns this node and the vanilla chain must not
	// act on it; a non-empty result is the name the chain evaluates (the
	// unmodified name for unflagged scenes and unmarked nodes).
	return chain_name;
}

Dictionary SHEMarkerInterpreter::marker_import_plan(Node *p_scene_root) const {
	Dictionary plan;
	Dictionary settings_by_key;
	Vector<String> errors;
	she_sdk::build_import_plan(p_scene_root, settings_by_key, errors);

	PackedStringArray error_array;
	for (const String &error : errors) {
		error_array.push_back(error);
	}
	plan["settings"] = settings_by_key;
	plan["errors"] = error_array;
	return plan;
}

void SHEMarkerInterpreter::apply_visual_drops(Node *p_scene_root) const {
	she_sdk::apply_visual_drops(p_scene_root);
}

void SHEMarkerInterpreter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("scene_is_she", "scene_root"), &SHEMarkerInterpreter::scene_is_she);
	ClassDB::bind_method(D_METHOD("has_she_markers", "node"), &SHEMarkerInterpreter::has_she_markers);
	ClassDB::bind_method(D_METHOD("chain_name_for", "scene_root", "node", "name"), &SHEMarkerInterpreter::chain_name_for);
	ClassDB::bind_method(D_METHOD("marker_import_plan", "scene_root"), &SHEMarkerInterpreter::marker_import_plan);
	ClassDB::bind_method(D_METHOD("apply_visual_drops", "scene_root"), &SHEMarkerInterpreter::apply_visual_drops);
}
