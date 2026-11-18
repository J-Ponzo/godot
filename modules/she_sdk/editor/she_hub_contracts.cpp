/**************************************************************************/
/*  she_hub_contracts.cpp                                                  */
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

#include "she_hub_contracts.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/string/print_string.h"

namespace she_sdk {

static constexpr const char *SHE_PLAY_PREFIX = "[SHE-PLAY] ";
static constexpr const char *SHE_CATALOG_PREFIX = "[SHE-CATALOG] ";

static const String PLAY_SCHEMA = "she-play/1";
static const String CATALOG_SCHEMA = "she-catalog/1";
static const String PLAY_PATH = "res://play.json";
static const String CATALOG_PATH = "res://ingredients/catalog.json";
static const String LEVELS_DIR = "res://levels";

// The SHE project gate: the projects this module tracks pin the SHE
// renderer (the template does, the hub copies it); anything else the
// binary ever opens (any vanilla Godot project) is never written.
static bool _project_pins_she_renderer() {
	return String(GLOBAL_GET("rendering/renderer/rendering_method")) == "she_deferred";
}

// The level's imported scene, from the ENGINE's own record: the `path=`
// destination of the `levels/<id>/<id>.blend.import` remap (the imported
// scenes live in `.godot/imported/`, never next to the .blend — same
// resolution the Blender add-on's play flow performs). Empty when the
// level was never imported (no remap) or its source is gone; a remap
// whose destination is not a res:// scene is corrupt, reported loudly.
static String _imported_scene_of_level(const String &p_level_id) {
	const String base = LEVELS_DIR.path_join(p_level_id);
	if (!FileAccess::exists(base.path_join(p_level_id + ".blend"))) {
		return String();
	}
	const String remap_path = base.path_join(p_level_id + ".blend.import");
	if (!FileAccess::exists(remap_path)) {
		return String(); // Never imported through the engine: no entry.
	}
	Ref<FileAccess> remap = FileAccess::open(remap_path, FileAccess::READ);
	if (remap.is_null()) {
		WARN_PRINT(vformat("%sCannot read the import remap '%s'.",
				SHE_PLAY_PREFIX, remap_path));
		return String();
	}
	while (!remap->eof_reached()) {
		const String line = remap->get_line().strip_edges();
		if (!line.begins_with("path=")) {
			continue;
		}
		String value = line.substr(5, line.length() - 5).strip_edges();
		if (value.begins_with("\"") && value.ends_with("\"") && value.length() >= 2) {
			value = value.substr(1, value.length() - 2);
		}
		if (value.begins_with("res://") && value.ends_with(".scn")) {
			return value;
		}
		WARN_PRINT(vformat("%sCorrupt import remap '%s' (scene destination: '%s').",
				SHE_PLAY_PREFIX, remap_path, value));
		return String();
	}
	return String();
}

// Writes the derived document only when its content differs from the
// file on disk (scans without SHE-relevant changes never touch mtimes,
// and concurrent editor sessions converge instead of churning).
static bool _write_document_if_changed(const char *p_prefix, const String &p_path,
		const String &p_content) {
	String existing;
	{
		Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
		if (file.is_valid()) {
			existing = file->get_as_text();
		}
	}
	if (existing == p_content) {
		return false;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		WARN_PRINT(vformat("%sCannot write '%s'.", p_prefix, p_path));
		return false;
	}
	file->store_string(p_content);
	file->close();
	return true;
}

void regenerate_play_catalog() {
	if (!_project_pins_she_renderer()) {
		return;
	}
	Ref<DirAccess> levels = DirAccess::open(LEVELS_DIR);
	if (levels.is_null()) {
		return; // No levels tree: not a project this contract belongs to.
	}
	Vector<String> level_ids;
	levels->list_dir_begin();
	String name = levels->get_next();
	while (!name.is_empty()) {
		if (name != "." && name != ".." && levels->current_is_dir()) {
			level_ids.push_back(name);
		}
		name = levels->get_next();
	}
	levels->list_dir_end();
	level_ids.sort();

	Array entries;
	int imported_levels = 0;
	for (const String &level_id : level_ids) {
		const String scene = _imported_scene_of_level(level_id);
		if (scene.is_empty()) {
			continue;
		}
		Dictionary entry;
		entry["name"] = level_id;
		entry["scene"] = scene;
		entry["origin"] = "level"; // The programmed "user" origin is Phase C.
		entries.push_back(entry);
		imported_levels++;
	}

	Dictionary play;
	play["schema"] = PLAY_SCHEMA;
	play["entries"] = entries;
	if (_write_document_if_changed(SHE_PLAY_PREFIX, PLAY_PATH,
			JSON::stringify(play, "\t"))) {
		print_line(vformat("%sRegenerated '%s' (%d imported level entries).",
				SHE_PLAY_PREFIX, PLAY_PATH, imported_levels));
	}

	Dictionary catalog;
	catalog["schema"] = CATALOG_SCHEMA;
	catalog["ingredients"] = Array(); // Placeholder (batch 2C fills the reflection).
	if (_write_document_if_changed(SHE_CATALOG_PREFIX, CATALOG_PATH,
			JSON::stringify(catalog, "\t"))) {
		print_line(vformat("%sRegenerated '%s' (placeholder, batch 2C fills it).",
				SHE_CATALOG_PREFIX, CATALOG_PATH));
	}
}

} // namespace she_sdk
