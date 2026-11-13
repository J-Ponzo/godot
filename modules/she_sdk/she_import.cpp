/**************************************************************************/
/*  she_import.cpp                                                        */
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

#include "she_import.h"

#include "scene/3d/importer_mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/physics/static_body_3d.h"

namespace she_sdk {
namespace {

// Marker axes (chantier `sdk`, design.md "Marqueurs d'import"). Values mirror
// the native importer node settings; unknown `she_*` keys are ignored (the
// namespace is shared with later batches: she_ingredient, she_light_*...).
constexpr const char *PROP_COLLISION = "she_collision";
constexpr const char *PROP_COLLISION_SHAPE = "she_collision_shape";
constexpr const char *PROP_OCCLUDER = "she_occluder";
constexpr const char *PROP_NAVMESH = "she_navmesh";
constexpr const char *PROP_VISUAL = "she_visual";
constexpr const char *PROP_NOIMP = "she_noimp";

enum CollisionAxis {
	COLLISION_ABSENT,
	COLLISION_NONE,
	COLLISION_STATIC,
	COLLISION_RIGID,
	COLLISION_AREA,
};

enum ShapeAxis {
	SHAPE_ABSENT,
	SHAPE_TRIMESH,
	SHAPE_CONVEX,
};

enum NavmeshAxis {
	NAV_ABSENT,
	NAV_OFF,
	NAV_MESH_AND_NAV,
	NAV_ONLY,
};

enum VisualAxis {
	VISUAL_ABSENT,
	VISUAL_KEEP,
	VISUAL_DROP,
};

struct Markers {
	CollisionAxis collision = COLLISION_ABSENT;
	ShapeAxis shape = SHAPE_ABSENT;
	bool occluder = false;
	NavmeshAxis navmesh = NAV_ABSENT;
	VisualAxis visual = VISUAL_ABSENT;
	bool noimp = false;
};

// Node settings keys and values of the native importer (resource_importer_scene).
constexpr const char *SETTING_GENERATE_PHYSICS = "generate/physics";
constexpr const char *SETTING_BODY_TYPE = "physics/body_type";
constexpr const char *SETTING_SHAPE_TYPE = "physics/shape_type";
constexpr const char *SETTING_GENERATE_NAVMESH = "generate/navmesh";
constexpr const char *SETTING_GENERATE_OCCLUDER = "generate/occluder";

const int BODY_TYPE_STATIC = 0; // ResourceImporterScene::BODY_TYPE_STATIC.
const int BODY_TYPE_DYNAMIC = 1; // ResourceImporterScene::BODY_TYPE_DYNAMIC.
const int BODY_TYPE_AREA = 2; // ResourceImporterScene::BODY_TYPE_AREA.
const int SHAPE_TYPE_SIMPLE_CONVEX = 1; // ResourceImporterScene::SHAPE_TYPE_SIMPLE_CONVEX.
const int SHAPE_TYPE_TRIMESH = 2; // ResourceImporterScene::SHAPE_TYPE_TRIMESH.
const int NAVMESH_MESH_AND_NAVMESH = 1; // ResourceImporterScene::NAVMESH_MESH_AND_NAVMESH.
const int NAVMESH_NAVMESH_ONLY = 2; // ResourceImporterScene::NAVMESH_NAVMESH_ONLY.
const int OCCLUDER_MESH_AND_OCCLUDER = 1; // ResourceImporterScene::OCCLUDER_MESH_AND_OCCLUDER.

bool _get_extras(const Node *p_node, Dictionary &r_extras) {
	if (!p_node->has_meta("extras")) {
		return false;
	}
	Variant extras = p_node->get_meta("extras");
	if (extras.get_type() != Variant::DICTIONARY) {
		return false;
	}
	r_extras = (Dictionary)extras;
	return true;
}

// The scene flag of the gateway: transported Blender -> glTF -> Godot by the
// additive GLTFDocument scene-extras channel (see she_import.h). Everything
// the module does is gated on this; no flag, no interpretation at all.
bool _scene_is_she(const Node *p_scene_root) {
	if (p_scene_root == nullptr || !p_scene_root->has_meta("gltf_scene_extras")) {
		return false;
	}
	Variant extras = p_scene_root->get_meta("gltf_scene_extras");
	if (extras.get_type() != Variant::DICTIONARY) {
		return false;
	}
	Dictionary dict = (Dictionary)extras;
	Variant flag = dict.get("she_scene", Variant());
	return flag.get_type() == Variant::BOOL && (bool)flag;
}

bool _has_she_key(const Dictionary &p_extras) {
	for (const KeyValue<Variant, Variant> &kv : p_extras) {
		if (String(kv.key).begins_with("she_")) {
			return true;
		}
	}
	return false;
}

int _string_axis_index(const char *const *p_allowed, int p_count, const Variant &p_value) {
	if (p_value.get_type() != Variant::STRING) {
		return -1;
	}
	String value = (String)p_value;
	for (int i = 0; i < p_count; i++) {
		if (value == String(p_allowed[i])) {
			return i;
		}
	}
	return -1;
}

Markers _parse_markers(const Dictionary &p_extras, bool p_strict, const String &p_where, Vector<String> &r_errors) {
	Markers m;
	static const char *collision_values[4] = { "none", "static", "rigid", "area" };
	static const char *shape_values[2] = { "trimesh", "convex" };
	static const char *navmesh_values[3] = { "off", "mesh+nav", "only" };
	static const char *visual_values[2] = { "keep", "drop" };

	for (const KeyValue<Variant, Variant> &kv : p_extras) {
		String key = String(kv.key);
		if (!key.begins_with("she_")) {
			continue;
		}
		if (key == PROP_COLLISION) {
			int idx = _string_axis_index(collision_values, 4, kv.value);
			if (idx >= 0) {
				m.collision = CollisionAxis(idx + 1);
			} else if (p_strict) {
				r_errors.push_back(vformat("%s: '%s' has an invalid value: %s (expected one of none, static, rigid, area)", p_where, key, String(Variant(kv.value))));
			}
		} else if (key == PROP_COLLISION_SHAPE) {
			int idx = _string_axis_index(shape_values, 2, kv.value);
			if (idx >= 0) {
				m.shape = ShapeAxis(idx + 1);
			} else if (p_strict) {
				r_errors.push_back(vformat("%s: '%s' has an invalid value: %s (expected trimesh or convex)", p_where, key, String(Variant(kv.value))));
			}
		} else if (key == PROP_OCCLUDER) {
			if (kv.value.get_type() == Variant::BOOL) {
				m.occluder = (bool)kv.value;
			} else if (p_strict) {
				r_errors.push_back(vformat("%s: '%s' must be a boolean, got %s", p_where, key, Variant::get_type_name(kv.value.get_type())));
			}
		} else if (key == PROP_NAVMESH) {
			int idx = _string_axis_index(navmesh_values, 3, kv.value);
			if (idx >= 0) {
				m.navmesh = NavmeshAxis(idx + 1);
			} else if (p_strict) {
				r_errors.push_back(vformat("%s: '%s' has an invalid value: %s (expected off, mesh+nav or only)", p_where, key, String(Variant(kv.value))));
			}
		} else if (key == PROP_VISUAL) {
			int idx = _string_axis_index(visual_values, 2, kv.value);
			if (idx >= 0) {
				m.visual = VisualAxis(idx + 1);
			} else if (p_strict) {
				r_errors.push_back(vformat("%s: '%s' has an invalid value: %s (expected keep or drop)", p_where, key, String(Variant(kv.value))));
			}
		} else if (key == PROP_NOIMP) {
			if (kv.value.get_type() == Variant::BOOL) {
				m.noimp = (bool)kv.value;
			} else if (p_strict) {
				r_errors.push_back(vformat("%s: '%s' must be a boolean, got %s", p_where, key, Variant::get_type_name(kv.value.get_type())));
			}
		}
		// Unknown `she_*` axes belong to later batches and stay inert here;
		// they keep traveling verbatim through the extras channel.
	}
	return m;
}

bool _has_role(const Markers &m) {
	return m.collision == COLLISION_STATIC || m.collision == COLLISION_RIGID || m.collision == COLLISION_AREA || m.occluder || m.navmesh == NAV_MESH_AND_NAV || m.navmesh == NAV_ONLY;
}

VisualAxis _visual_effective(const Markers &m) {
	if (m.visual != VISUAL_ABSENT) {
		return m.visual;
	}
	// Defaults per role (design.md "Sélection SHE"): rigid keeps its mesh
	// (natively forced by RigidBody3D), area and navmesh-only drop it
	// (natively destructive node replacements), static/occluder are
	// deterministic scenery (drop), mesh+nav keeps the floor visible.
	if (m.collision == COLLISION_RIGID) {
		return VISUAL_KEEP;
	}
	if (m.collision == COLLISION_AREA || m.navmesh == NAV_ONLY) {
		return VISUAL_DROP;
	}
	if (m.collision == COLLISION_STATIC || m.occluder) {
		return VISUAL_DROP;
	}
	return VISUAL_KEEP;
}

// The marker states that a single native suffix branch can execute on its own:
// static + drop (with the trimesh/convex shape refinement) and occluder + drop.
// Anything else is executed by the settings channel and/or the module-side
// drop pass (see she_import.h). Returns the suffix token to append, or "".
String _native_drop_single_suffix(const Markers &m) {
	bool nav_clean = (m.navmesh == NAV_ABSENT || m.navmesh == NAV_OFF);
	if (!nav_clean) {
		return String();
	}
	if (m.collision == COLLISION_STATIC && !m.occluder && _visual_effective(m) == VISUAL_DROP) {
		return (m.shape == SHAPE_CONVEX) ? "-convcolonly" : "-colonly";
	}
	if (m.occluder && (m.collision == COLLISION_ABSENT || m.collision == COLLISION_NONE) && _visual_effective(m) == VISUAL_DROP) {
		return "-occonly";
	}
	return String();
}

// Fail-loudly marker matrix, identical at every layer (runner, module). One
// message per violated rule; the caller aborts the import when non-empty.
void _validate_matrix(const Markers &m, bool p_is_mesh, const String &p_where, Vector<String> &r_errors) {
	if (m.noimp && _has_role(m)) {
		r_errors.push_back(vformat("%s: '%s' cannot be combined with a role", p_where, PROP_NOIMP));
	}
	if (!p_is_mesh) {
		// Role and visual markers are mesh-only (design.md).
		if (m.collision != COLLISION_ABSENT) {
			r_errors.push_back(vformat("%s: '%s' is mesh-only", p_where, PROP_COLLISION));
		}
		if (m.shape != SHAPE_ABSENT) {
			r_errors.push_back(vformat("%s: '%s' is mesh-only", p_where, PROP_COLLISION_SHAPE));
		}
		if (m.occluder) {
			r_errors.push_back(vformat("%s: '%s' is mesh-only", p_where, PROP_OCCLUDER));
		}
		if (m.navmesh != NAV_ABSENT) {
			r_errors.push_back(vformat("%s: '%s' is mesh-only", p_where, PROP_NAVMESH));
		}
		if (m.visual != VISUAL_ABSENT) {
			r_errors.push_back(vformat("%s: '%s' is mesh-only", p_where, PROP_VISUAL));
		}
		return;
	}
	if (m.collision == COLLISION_NONE && m.shape != SHAPE_ABSENT) {
		r_errors.push_back(vformat("%s: '%s' requires a collision role ('%s' is none)", p_where, PROP_COLLISION_SHAPE, PROP_COLLISION));
	}
	if (m.visual == VISUAL_DROP && !_has_role(m)) {
		r_errors.push_back(vformat("%s: '%s=drop' without a role: nothing would survive the import", p_where, PROP_VISUAL));
	}
	if (m.visual == VISUAL_DROP && m.collision == COLLISION_RIGID) {
		r_errors.push_back(vformat("%s: rigid bodies keep their mesh ('%s=keep' is native)", p_where, PROP_VISUAL));
	}
	if (m.visual == VISUAL_KEEP && (m.collision == COLLISION_AREA || m.navmesh == NAV_ONLY)) {
		r_errors.push_back(vformat("%s: area / navmesh-only natively destroy the mesh ('%s=keep' is impossible)", p_where, PROP_VISUAL));
	}
	int destructive = 0;
	if (m.collision == COLLISION_AREA) {
		destructive++;
	}
	if (m.navmesh == NAV_ONLY) {
		destructive++;
	}
	if (destructive > 1) {
		r_errors.push_back(vformat("%s: at most one destructive mode per node ('%s=area' + '%s=only')", p_where, PROP_COLLISION, PROP_NAVMESH));
	}
	if (m.occluder) {
		// Node settings order (physics, then navmesh, then occluder, each
		// requiring the node to still be an ImporterMeshInstance3D) - "zéro
		// invention": only combos the native settings code can express are legal.
		if (m.collision == COLLISION_RIGID || m.collision == COLLISION_AREA) {
			r_errors.push_back(vformat("%s: '%s' cannot compose with a rigid/area collision body (native settings order)", p_where, PROP_OCCLUDER));
		}
		if (m.navmesh == NAV_ONLY) {
			r_errors.push_back(vformat("%s: '%s' cannot compose with '%s=only' (native settings order)", p_where, PROP_OCCLUDER, PROP_NAVMESH));
		}
	}
	if ((m.collision == COLLISION_RIGID || m.collision == COLLISION_AREA) && m.navmesh == NAV_MESH_AND_NAV) {
		r_errors.push_back(vformat("%s: rigid/area collision bodies cannot compose with '%s=mesh+nav' (native settings order)", p_where, PROP_NAVMESH));
	}
}

// Marker axes -> native node settings (the `_subresources` "nodes" channel).
Dictionary _compute_settings(const Markers &m) {
	Dictionary d;
	if (m.collision == COLLISION_STATIC || m.collision == COLLISION_RIGID || m.collision == COLLISION_AREA) {
		d[SETTING_GENERATE_PHYSICS] = true;
		d[SETTING_BODY_TYPE] = (m.collision == COLLISION_STATIC) ? BODY_TYPE_STATIC : ((m.collision == COLLISION_RIGID) ? BODY_TYPE_DYNAMIC : BODY_TYPE_AREA);
		switch (m.shape) {
			case SHAPE_TRIMESH:
				d[SETTING_SHAPE_TYPE] = SHAPE_TYPE_TRIMESH;
				break;
			case SHAPE_CONVEX:
				d[SETTING_SHAPE_TYPE] = SHAPE_TYPE_SIMPLE_CONVEX;
				break;
			default:
				break; // Absent shape: the native default (Automatic) applies.
		}
	}
	if (m.occluder) {
		d[SETTING_GENERATE_OCCLUDER] = OCCLUDER_MESH_AND_OCCLUDER;
	}
	switch (m.navmesh) {
		case NAV_MESH_AND_NAV:
			d[SETTING_GENERATE_NAVMESH] = NAVMESH_MESH_AND_NAVMESH;
			break;
		case NAV_ONLY:
			d[SETTING_GENERATE_NAVMESH] = NAVMESH_NAVMESH_ONLY;
			break;
		default:
			break;
	}
	return d;
}

void _prepare_walk(Node *p_node, Node *p_root, Dictionary &r_settings_by_key, Vector<String> &r_errors) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_prepare_walk(p_node->get_child(i), p_root, r_settings_by_key, r_errors);
	}

	Dictionary extras;
	if (!_get_extras(p_node, extras) || !_has_she_key(extras)) {
		return;
	}
	String where = String(p_root->get_path_to(p_node));
	Markers m = _parse_markers(extras, true, where, r_errors);

	if (Object::cast_to<ImporterMeshInstance3D>(p_node)) {
		_validate_matrix(m, true, where, r_errors);
		Dictionary settings = _compute_settings(m);
		if (!settings.is_empty()) {
			// Replicate the importer's node-settings lookup key exactly:
			// the node's `import_id` meta, or "PATH:<path from scene root>".
			String key = "PATH:" + where;
			if (p_node->has_meta("import_id")) {
				Variant import_id = p_node->get_meta("import_id");
				if (import_id.get_type() == Variant::STRING) {
					key = (String)import_id;
				}
			}
			Dictionary merged;
			if (r_settings_by_key.has(key)) {
				Variant existing = r_settings_by_key[key];
				if (existing.get_type() == Variant::DICTIONARY) {
					merged = (Dictionary)existing.duplicate(true);
				}
			}
			// Marker priority: marker-driven role keys are authoritative over
			// any hand-configured node settings for the same object. The
			// importer merges this plan the same way.
			for (const KeyValue<Variant, Variant> &kv : settings) {
				merged[kv.key] = kv.value;
			}
			r_settings_by_key[key] = merged;
		}
		return;
	}

	if (Object::cast_to<StaticBody3D>(p_node)) {
		// Legitimate chain artifact: a static + drop single was consumed by
		// the "-colonly"-family branch, which copies the markers onto the
		// derived StaticBody3D. Nothing left to interpret here.
		return;
	}
	_validate_matrix(m, false, where, r_errors);
}

bool _should_drop(Node *p_node) {
	if (Object::cast_to<ImporterMeshInstance3D>(p_node) == nullptr) {
		return false; // Drop singles were consumed by the chain (type changed or node deleted).
	}
	Dictionary extras;
	if (!_get_extras(p_node, extras) || !_has_she_key(extras)) {
		return false;
	}
	Vector<String> ignored;
	Markers m = _parse_markers(extras, false, String(), ignored);
	return _has_role(m) && _visual_effective(m) == VISUAL_DROP;
}

void _drop_mesh_node(Node *p_victim, Node *p_new_parent) {
	Node3D *mesh_node = Object::cast_to<Node3D>(p_victim);
	ERR_FAIL_NULL(mesh_node);
	Transform3D mesh_transform = mesh_node->get_transform();
	int count = p_victim->get_child_count();
	for (int i = 0; i < count; i++) {
		Node *child = p_victim->get_child(0);
		// Keep the owner through the reparent (unset during the move to avoid
		// the mid-move owner-consistency warning, restore afterwards), so the
		// derivatives still belong to the packed scene at their new place.
		Node *child_owner = child->get_owner();
		child->set_owner(nullptr);
		p_victim->remove_child(child);
		p_new_parent->add_child(child, true);
		if (child_owner) {
			child->set_owner(child_owner);
		}
		Node3D *child_3d = Object::cast_to<Node3D>(child);
		if (child_3d) {
			// Derivatives keep their place: the former mesh node's local
			// transform is composed into the reparented child.
			child_3d->set_transform(mesh_transform * child_3d->get_transform());
		} else {
			ERR_PRINT(vformat("SHE markers: dropping '%s' but child '%s' is not a Node3D; it is kept with an uncompensated transform.", p_victim->get_name(), child->get_name()));
		}
	}
	p_victim->set_owner(nullptr);
	memdelete(p_victim);
}

void _drop_walk(Node *p_node) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		_drop_walk(child);
		if (_should_drop(child)) {
			_drop_mesh_node(child, p_node);
			i--;
		}
	}
}

// Gateway audit: every node carrying at least one `she_*` property in a
// scene without the `she_scene` flag is listed fail-loudly (one message per
// object), and the plan stays empty - everything runs vanilla.
void _audit_markers_outside_she_scene(Node *p_node, Node *p_root, Vector<String> &r_errors) {
	Dictionary extras;
	if (_get_extras(p_node, extras) && _has_she_key(extras)) {
		r_errors.push_back(vformat("%s: SHE markers in a non-SHE scene (missing 'she_scene' flag on the scene)", p_root->get_path_to(p_node)));
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_audit_markers_outside_she_scene(p_node->get_child(i), p_root, r_errors);
	}
}

} // namespace

bool scene_is_she(const Node *p_scene_root) {
	return _scene_is_she(p_scene_root);
}

bool has_she_markers(const Node *p_node) {
	Dictionary extras;
	return _get_extras(p_node, extras) && _has_she_key(extras);
}

bool resolve_chain_name(const Node *p_scene_root, const Node *p_node, String &r_chain_name) {
	if (!_scene_is_she(p_scene_root)) {
		// Gateway: outside a flagged scene the module interprets nothing -
		// the chain stays purely vanilla for every node, `she_*` properties
		// are inert data (parsing them is the plan's job, which reports
		// markers-in-an-unflagged-scene fail-loudly).
		return false;
	}
	if (!has_she_markers(p_node)) {
		return false; // Unmarked: the suffixed-name chain stays purely vanilla.
	}
	// Marked: the module owns this node's chain channel.
	if (Object::cast_to<ImporterMeshInstance3D>(p_node) == nullptr) {
		r_chain_name = String();
		return true;
	}
	Vector<String> ignored;
	Dictionary extras;
	_get_extras(p_node, extras);
	Markers m = _parse_markers(extras, false, String(), ignored);
	String suffix = _native_drop_single_suffix(m);
	if (!suffix.is_empty()) {
		// Virtual suffix: the vanilla branch chain executes this state below,
		// renames included (dump parity with an upstream suffixed object).
		r_chain_name = r_chain_name + suffix;
	} else {
		// Empty name: no branch of the vanilla chain can match (including
		// the implicit -noimp deletion); settings and/or the drop pass own it.
		r_chain_name = String();
	}
	return true;
}

void build_import_plan(Node *p_scene_root, Dictionary &r_settings_by_key, Vector<String> &r_errors) {
	if (!_scene_is_she(p_scene_root)) {
		// Gateway: markers present in an unflagged scene are a configuration
		// error (the runner fires first at export; this is the import-side
		// barriere of the same matrix). One message per offending object,
		// no settings - the plan stays empty and everything stays vanilla.
		_audit_markers_outside_she_scene(p_scene_root, p_scene_root, r_errors);
		return;
	}
	_prepare_walk(p_scene_root, p_scene_root, r_settings_by_key, r_errors);
}

void apply_visual_drops(Node *p_scene_root) {
	if (!_scene_is_she(p_scene_root)) {
		return; // Gateway: nothing is interpreted outside a flagged scene.
	}
	_drop_walk(p_scene_root);
}

} // namespace she_sdk
