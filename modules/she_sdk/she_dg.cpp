/**************************************************************************/
/*  she_dg.cpp                                                            */
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

#include "she_dg.h"

#include "she_dg_buffer.h"
#include "she_import.h"

#include "core/error/error_macros.h"
#include "core/io/config_file.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/math_funcs.h"
#include "core/io/file_access.h"
#include "core/string/print_string.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/node.h"

namespace she_sdk {
namespace {

const char *const SHE_DG_PREFIX = "[SHE-DG] ";

// The DGBuffer map slots, in the bake descriptor's map naming. These four
// are the whole 2A3 contract (emission/light AOVs belong to later phases).
const char *const DG_MAP_SLOTS[] = { "depth", "albedo", "normal", "orm" };
const int DG_MAP_SLOT_COUNT = 4;

struct BakeCamera {
	String name;
	double clip_near = 0.0;
	double clip_far = 0.0;
	HashMap<String, String> map_files; // Map name -> flat file name in dg/.
};

bool _get_number(const Variant &p_value, double &r_out) {
	if (p_value.get_type() == Variant::FLOAT || p_value.get_type() == Variant::INT) {
		r_out = (double)p_value;
		return true;
	}
	return false;
}

void _collect_cameras(Node *p_node, List<Camera3D *> &r_cameras) {
	Camera3D *camera = Object::cast_to<Camera3D>(p_node);
	if (camera) {
		r_cameras.push_back(camera);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_cameras(p_node->get_child(i), r_cameras);
	}
}

// Parses dg/bake.json (schema she-bake/1). A malformed descriptor is
// reported loudly and returns false; the cameras that did validate stay
// in the list (a partial generation is worse than none, but one bad
// camera must not silently disable the others). The caller handles the
// missing-file case (a legitimate unbaked work-in-progress level).
bool _parse_bake_descriptor(const String &p_bake_path, List<BakeCamera> &r_cameras) {
	Error err = OK;
	const String text = FileAccess::get_file_as_string(p_bake_path, &err);
	if (err != OK) {
		ERR_PRINT(vformat("%sCannot read the bake descriptor '%s'.", SHE_DG_PREFIX, p_bake_path));
		return false;
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(text) != OK) {
		ERR_PRINT(vformat("%sInvalid JSON in the bake descriptor '%s'.", SHE_DG_PREFIX, p_bake_path));
		return false;
	}
	const Variant data = json->get_data();
	if (data.get_type() != Variant::DICTIONARY) {
		ERR_PRINT(vformat("%sBake descriptor '%s' is not an object.", SHE_DG_PREFIX, p_bake_path));
		return false;
	}
	const Dictionary bake = data;
	const Variant schema = bake.get("schema", Variant());
	if (schema.get_type() != Variant::STRING || String(schema) != "she-bake/1") {
		ERR_PRINT(vformat("%sBake descriptor '%s' is not a she-bake/1 document (schema key: %s).", SHE_DG_PREFIX, p_bake_path, schema));
		return false;
	}
	const Variant cameras_v = bake.get("cameras", Variant());
	if (cameras_v.get_type() != Variant::ARRAY) {
		ERR_PRINT(vformat("%sBake descriptor '%s' has no 'cameras' array.", SHE_DG_PREFIX, p_bake_path));
		return false;
	}
	const Array cameras = cameras_v;

	bool all_valid = true;
	for (int i = 0; i < cameras.size(); i++) {
		const Variant camera_v = cameras[i];
		if (camera_v.get_type() != Variant::DICTIONARY) {
			ERR_PRINT(vformat("%sBake descriptor '%s': camera #%d is not an object.", SHE_DG_PREFIX, p_bake_path, i));
			all_valid = false;
			continue;
		}
		const Dictionary camera = camera_v;
		const Variant name_v = camera.get("name", Variant());
		if (name_v.get_type() != Variant::STRING || String(name_v).is_empty()) {
			ERR_PRINT(vformat("%sBake descriptor '%s': camera #%d has no name.", SHE_DG_PREFIX, p_bake_path, i));
			all_valid = false;
			continue;
		}
		BakeCamera bake_camera;
		bake_camera.name = name_v;

		const Variant clip_v = camera.get("clip", Variant());
		Array clip;
		if (clip_v.get_type() == Variant::ARRAY) {
			clip = clip_v;
		}
		if (clip.size() != 2 || !_get_number(clip[0], bake_camera.clip_near) || !_get_number(clip[1], bake_camera.clip_far)) {
			ERR_PRINT(vformat("%sBake descriptor '%s': camera '%s' has no '[near, far]' clip.", SHE_DG_PREFIX, p_bake_path, bake_camera.name));
			all_valid = false;
			continue;
		}

		// The runtime init pass hardcodes the [0, 1] shell remap semantics
		// of the bake (she_dg_init.glsl); any other z_to would silently
		// misdecode the depth map.
		double z_to[2] = { 0.0, 1.0 };
		const Variant z_to_v = camera.get("z_to", Variant());
		Array z_to_array;
		if (z_to_v.get_type() == Variant::ARRAY) {
			z_to_array = z_to_v;
		}
		if (z_to_array.size() != 2 || !_get_number(z_to_array[0], z_to[0]) || !_get_number(z_to_array[1], z_to[1]) || z_to[0] != 0.0 || z_to[1] != 1.0) {
			ERR_PRINT(vformat("%sBake descriptor '%s': camera '%s' does not use the [0, 1] z remap the G-buffer init decodes (z_to).", SHE_DG_PREFIX, p_bake_path, bake_camera.name));
			all_valid = false;
			continue;
		}

		const Variant maps_v = camera.get("maps", Variant());
		if (maps_v.get_type() != Variant::ARRAY) {
			ERR_PRINT(vformat("%sBake descriptor '%s': camera '%s' has no 'maps' array.", SHE_DG_PREFIX, p_bake_path, bake_camera.name));
			all_valid = false;
			continue;
		}
		const Array maps = maps_v;
		for (int m = 0; m < maps.size(); m++) {
			const Variant map_v = maps[m];
			if (map_v.get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary map = map_v;
			const Variant map_name = map.get("name", Variant());
			const Variant map_file = map.get("file", Variant());
			if (map_name.get_type() == Variant::STRING && map_file.get_type() == Variant::STRING) {
				bake_camera.map_files.insert(String(map_name), String(map_file));
			}
		}
		r_cameras.push_back(bake_camera);
	}
	return all_valid;
}

} // namespace

void apply_dg_post_import(Node *p_scene_root, const String &p_source_file) {
	if (p_scene_root == nullptr || !scene_is_she(p_scene_root)) {
		return;
	}
	// The dg/ directory is the sibling of the imported scene source
	// (levels/<id>/dg/ in the project doctrine). No descriptor: an unbaked
	// work-in-progress level, nothing to generate or bind.
	const String dg_dir = p_source_file.get_base_dir().path_join("dg");
	const String bake_path = dg_dir.path_join("bake.json");
	if (!FileAccess::exists(bake_path)) {
		return;
	}

	List<BakeCamera> bake_cameras;
	_parse_bake_descriptor(bake_path, bake_cameras);

	HashSet<String> map_files;
	for (const BakeCamera &bake_camera : bake_cameras) {
		for (const KeyValue<String, String> &kv : bake_camera.map_files) {
			map_files.insert(kv.value);
		}
	}

	// Route A (acté au cadrage 2A3): pin the float-exact texture import
	// options in the .import of every map of the bake, then let the native
	// "a modified .import forces the reimport" mechanism apply them at the
	// next scan (proven by the 2A2 settings family). The default EXR import
	// is already float-exact (the Lossless mode auto-remaps float formats
	// to VRAM Uncompressed); the patch neutralizes the detect_3d flip and
	// pins the mode. Only rewrite when a value actually changes: an
	// identical write would flag spurious reimports on every scene
	// reimport.
	int pinned_count = 0;
	int unimported_maps = 0;
	for (const String &map_file : map_files) {
		const String exr_path = dg_dir.path_join(map_file);
		const String import_path = exr_path + ".import";
		if (!FileAccess::exists(exr_path)) {
			continue;
		}
		if (!FileAccess::exists(import_path)) {
			// Fresh scan ordering: the map has not been imported yet. Its
			// default import is already float-exact for the bake formats;
			// the pin applies at the next import of the scene source.
			unimported_maps++;
			continue;
		}
		ConfigFile import_file;
		if (import_file.load(import_path) != OK) {
			ERR_PRINT(vformat("%sCannot parse '%s' to pin the import options.", SHE_DG_PREFIX, import_path));
			continue;
		}
		bool dirty = false;
		if ((int)import_file.get_value("params", "compress/mode", -1) != 0) {
			import_file.set_value("params", "compress/mode", 0);
			dirty = true;
		}
		if ((int)import_file.get_value("params", "detect_3d/compress_to", -1) != 0) {
			import_file.set_value("params", "detect_3d/compress_to", 0);
			dirty = true;
		}
		if (!dirty) {
			continue;
		}
		if (import_file.save(import_path) != OK) {
			ERR_PRINT(vformat("%sCannot write the import options into '%s'.", SHE_DG_PREFIX, import_path));
		} else {
			pinned_count++;
		}
	}

	List<Camera3D *> scene_cameras;
	_collect_cameras(p_scene_root, scene_cameras);

	int generated_count = 0;
	int bound_count = 0;
	for (const BakeCamera &bake_camera : bake_cameras) {
		bool generate_failed = false;
		Ref<Texture2D> slots[DG_MAP_SLOT_COUNT];
		for (int s = 0; s < DG_MAP_SLOT_COUNT; s++) {
			const String slot = DG_MAP_SLOTS[s];
			const String *file = bake_camera.map_files.getptr(slot);
			if (file == nullptr || !FileAccess::exists(dg_dir.path_join(*file))) {
				ERR_PRINT(vformat("%sCamera '%s': no '%s' map in the bake descriptor or on disk - no DGBuffer generated.", SHE_DG_PREFIX, bake_camera.name, slot));
				generate_failed = true;
				break;
			}
			const String exr_path = dg_dir.path_join(*file);
			Ref<Texture2D> texture = ResourceLoader::load(exr_path, "Texture2D");
			if (texture.is_null()) {
				ERR_PRINT(vformat("%sCamera '%s': cannot load the '%s' map '%s'.", SHE_DG_PREFIX, bake_camera.name, slot, exr_path));
				generate_failed = true;
				break;
			}
			slots[s] = texture;
		}
		if (generate_failed) {
			continue;
		}

		Ref<DGBuffer> dg_buffer;
		dg_buffer.instantiate();
		dg_buffer->set_depth(slots[0]);
		dg_buffer->set_albedo(slots[1]);
		dg_buffer->set_normal(slots[2]);
		dg_buffer->set_orm(slots[3]);

		const String tres_path = dg_dir.path_join(bake_camera.name + ".tres");
		if (ResourceSaver::save(dg_buffer, tres_path) != OK) {
			ERR_PRINT(vformat("%sCamera '%s': cannot save the DGBuffer resource '%s'.", SHE_DG_PREFIX, bake_camera.name, tres_path));
			continue;
		}
		generated_count++;

		// Bind by name (the glTF node name carries the Blender camera
		// name). A baked camera without a matching imported node is normal
		// (e.g. a she_noimp camera) and stays silent. The clip coherence
		// is an exact comparison: both sides round the same Blender double
		// through float32, so any inequality is a diverged camera or a
		// stale bake - the depth remap of the maps would no longer match
		// the runtime camera, and the G-buffer init would silently
		// misdecode: the camera stays unbound and the error is reported.
		for (Camera3D *camera : scene_cameras) {
			if (String(camera->get_name()) != bake_camera.name) {
				continue;
			}
			if ((double)camera->get_near() != bake_camera.clip_near || (double)camera->get_far() != bake_camera.clip_far) {
				ERR_PRINT(vformat("%sCamera '%s': near/far (%s, %s) do not match the bake clip (%s, %s) - stale bake or diverged camera; DGBuffer not bound.", SHE_DG_PREFIX,
						bake_camera.name, rtos(camera->get_near()), rtos(camera->get_far()), rtos(bake_camera.clip_near), rtos(bake_camera.clip_far)));
				continue;
			}
			camera->set_dg_buffer(dg_buffer);
			bound_count++;
		}
	}

	print_line(vformat("%s%s: generated %d DGBuffer(s), bound %d camera(s), pinned %d import(s)%s.", SHE_DG_PREFIX, p_source_file, generated_count, bound_count, pinned_count,
			unimported_maps > 0 ? vformat(" (%d map(s) not imported yet - pinned on the next import of the scene source)", unimported_maps) : String()));
}

} // namespace she_sdk
