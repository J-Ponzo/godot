/**************************************************************************/
/*  editor_import_blend_runner.cpp                                        */
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

#include "editor_import_blend_runner.h"

#include "core/io/http_client.h"
#include "core/io/xml_parser.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"

static constexpr char PYTHON_SCRIPT_RPC[] = R"(
import bpy, sys, threading
from xmlrpc.server import SimpleXMLRPCServer
req = threading.Condition()
res = threading.Condition()
info = None
export_err = None
def xmlrpc_server():
  server = SimpleXMLRPCServer(('127.0.0.1', %d))
  server.register_function(export_gltf)
  server.serve_forever()
def export_gltf(opts):
  with req:
    global info
    info = ('export_gltf', opts)
    req.notify()
  with res:
    res.wait()
  if export_err:
    raise export_err
  # Important to return a value to prevent the error 'cannot marshal None unless allow_none is enabled'.
  return 'BLENDER_GODOT_EXPORT_SUCCESSFUL'
if bpy.app.version < (3, 0, 0):
  print('Blender 3.0 or higher is required.', file=sys.stderr)
threading.Thread(target=xmlrpc_server).start()
while True:
  with req:
    while info is None:
      req.wait()
  method, opts = info
  if method == 'export_gltf':
    try:
      export_err = None
      bpy.ops.wm.open_mainfile(filepath=opts['path'])
      if opts['unpack_all']:
        bpy.ops.file.unpack_all(method='USE_LOCAL')
      _she_select_scene_participants()
      bpy.ops.export_scene.gltf(**opts['gltf_options'])
    except Exception as e:
      export_err = e
  info = None
  with res:
    res.notify()
)";

static constexpr char PYTHON_SCRIPT_DIRECT[] = R"(
import bpy, sys
opts = %s
%s
if bpy.app.version < (3, 0, 0):
  print('Blender 3.0 or higher is required.', file=sys.stderr)
bpy.ops.wm.open_mainfile(filepath=opts['path'])
if opts['unpack_all']:
  bpy.ops.file.unpack_all(method='USE_LOCAL')
_she_select_scene_participants()
bpy.ops.export_scene.gltf(**opts['gltf_options'])
)";

// SHE SDK (chantier `sdk`, batch 2A2) - scene-level selection.
// Scene participation is governed by the `she_scene` custom property of the
// active scene (posed by the add-on): when it is absent the whole block is
// inert and the export runs byte-identical to the vanilla pipeline.
// When present, non-participant objects are unlinked from every collection of
// the level so they never enter the glTF: cameras and lights participate by
// default (opt-out via `she_noimp`), empties are skipped by default (opt-in
// via `she_noimp` explicitly false), meshes participate when they carry a role
// (`she_collision` / `she_occluder` / `she_navmesh`), keep their visual
// (`she_visual=keep`) or opt in explicitly (`she_noimp` explicitly false).
// The marker matrix below is the same fail-loudly matrix as the she_sdk
// module's import side (see modules/she_sdk/she_import.cpp): invalid states
// raise before any scene modification, with every faulty object listed.
// NOTE (engine convention): the embedded Python must not contain any double
// quote character - on Unix the blocking `execute()` goes through popen(),
// which wraps each argument in plain double quotes (the vanilla RPC/DIRECT
// scripts follow the same single-quote discipline). The raw string also uses
// the `she` delimiter on purpose: the Python contains `)"` sequences that
// would terminate a plain `R"(...)"`.
static constexpr char PYTHON_SCRIPT_SHE_SELECT[] = R"she(
_SHE_STR_AXES = {
  'she_collision': ('none', 'static', 'rigid', 'area'),
  'she_collision_shape': ('trimesh', 'convex'),
  'she_navmesh': ('off', 'mesh+nav', 'only'),
  'she_visual': ('keep', 'drop'),
}
_SHE_ROLE_KEYS = ('she_collision', 'she_collision_shape', 'she_occluder', 'she_navmesh', 'she_visual')

def _she_markers(obj):
  out = {}
  for key in obj.keys():
    if key.startswith('she_'):
      out[key] = obj[key]
  return out

def _she_has_role(m):
  if m.get('she_collision') in ('static', 'rigid', 'area'):
    return True
  if m.get('she_occluder') is True:
    return True
  if m.get('she_navmesh') in ('mesh+nav', 'only'):
    return True
  return False

def _she_participates(obj):
  m = _she_markers(obj)
  if m.get('she_noimp') is True:
    return False
  if obj.type == 'MESH':
    return _she_has_role(m) or (m.get('she_visual') == 'keep') or ('she_noimp' in m)
  if obj.type in ('CAMERA', 'LIGHT'):
    return True
  # Empties and other helper types: skipped by default (Phase C will make
  # she_ingredients explicitly participate), opt-in per object.
  return ('she_noimp' in m)

def _she_validate(obj, errors):
  m = _she_markers(obj)
  where = '%s' % obj.name
  def err(msg):
    errors.append('%s: %s' % (where, msg))
  for key, vals in _SHE_STR_AXES.items():
    if key in m and not (isinstance(m[key], str) and m[key] in vals):
      err('marker %s has an invalid value: %r' % (key, m[key]))
  for key in ('she_occluder', 'she_noimp'):
    if key in m and not isinstance(m[key], bool):
      err('marker %s must be a boolean' % key)
  col = m.get('she_collision')
  occ = (m.get('she_occluder') is True)
  nav = m.get('she_navmesh')
  vis = m.get('she_visual')
  if (m.get('she_noimp') is True) and _she_has_role(m):
    err('marker she_noimp cannot be combined with a role')
  if obj.type != 'MESH':
    for key in _SHE_ROLE_KEYS:
      if key in m:
        err('marker %s is mesh-only' % key)
    return
  if col == 'none' and ('she_collision_shape' in m):
    err('marker she_collision_shape requires a collision role (she_collision is none)')
  if vis == 'drop' and not _she_has_role(m):
    err('marker she_visual=drop without a role: nothing would survive the import')
  if vis == 'drop' and col == 'rigid':
    err('rigid bodies keep their mesh (she_visual=keep is native)')
  if vis == 'keep' and (col == 'area' or nav == 'only'):
    err('area / navmesh-only natively destroy the mesh (she_visual=keep is impossible)')
  destructive = (1 if col == 'area' else 0) + (1 if nav == 'only' else 0)
  if destructive > 1:
    err('at most one destructive mode per node')
  if occ:
    if col in ('rigid', 'area'):
      err('marker she_occluder cannot compose with a rigid/area collision body (native settings order)')
    if nav == 'only':
      err('marker she_occluder cannot compose with she_navmesh=only (native settings order)')
  if col in ('rigid', 'area') and nav == 'mesh+nav':
    err('rigid/area collision bodies cannot compose with she_navmesh=mesh+nav (native settings order)')

def _she_select_scene_participants():
  scene = bpy.context.scene
  if not scene.get('she_scene'):
    # Gateway (acte 2026-11-13): no flag => pure vanilla export, and any
    # `she_*` custom property in such a scene is a configuration error (the
    # prefix belongs to the SDK) - fail loudly with the objects listed, the
    # same matrix the import-side module applies.
    offenders = []
    for obj in list(scene.objects):
      if _she_markers(obj):
        offenders.append(obj.name)
    if offenders:
      raise RuntimeError('[SHE markers] non-SHE scene carries she_* properties: ' + '; '.join(offenders))
    return
  keep = set()
  errors = []
  for obj in list(scene.objects):
    _she_validate(obj, errors)
    if _she_participates(obj):
      keep.add(obj.name)
  if errors:
    raise RuntimeError('[SHE markers] invalid marker state: ' + '; '.join(errors))
  skipped = 0
  for obj in list(scene.objects):
    if obj.name not in keep:
      for coll in list(obj.users_collection):
        coll.objects.unlink(obj)
      skipped += 1
  print('[SHE-SELECT] scene %r: kept %d object(s), skipped %d' % (scene.name, len(keep), skipped))
)she";

String dict_to_python(const Dictionary &p_dict) {
	String entries;
	for (const KeyValue<Variant, Variant> &kv : p_dict) {
		const String &key = kv.key;
		String value;
		const Variant &raw_value = kv.value;

		switch (raw_value.get_type()) {
			case Variant::Type::BOOL: {
				value = raw_value ? "True" : "False";
				break;
			}
			case Variant::Type::STRING:
			case Variant::Type::STRING_NAME: {
				value = raw_value;
				value = vformat("'%s'", value.c_escape());
				break;
			}
			case Variant::Type::DICTIONARY: {
				value = dict_to_python(raw_value);
				break;
			}
			default: {
				ERR_FAIL_V_MSG("", vformat("Unhandled Variant type %s for python dictionary", Variant::get_type_name(raw_value.get_type())));
			}
		}

		entries += vformat("'%s': %s,", key, value);
	}
	return vformat("{%s}", entries);
}

String dict_to_xmlrpc(const Dictionary &p_dict) {
	String members;
	for (const KeyValue<Variant, Variant> &kv : p_dict) {
		const String &key = kv.key;
		String value;
		const Variant &raw_value = kv.value;

		switch (raw_value.get_type()) {
			case Variant::Type::BOOL: {
				value = vformat("<boolean>%d</boolean>", raw_value ? 1 : 0);
				break;
			}
			case Variant::Type::STRING:
			case Variant::Type::STRING_NAME: {
				value = raw_value;
				value = vformat("<string>%s</string>", value.xml_escape());
				break;
			}
			case Variant::Type::DICTIONARY: {
				value = dict_to_xmlrpc(raw_value);
				break;
			}
			default: {
				ERR_FAIL_V_MSG("", vformat("Unhandled Variant type %s for XMLRPC", Variant::get_type_name(raw_value.get_type())));
			}
		}

		members += vformat("<member><name>%s</name><value>%s</value></member>", key, value);
	}
	return vformat("<struct>%s</struct>", members);
}

Error EditorImportBlendRunner::start_blender(const String &p_python_script, bool p_blocking) {
	String blender_path = EDITOR_GET("filesystem/import/blender/blender_path");

	List<String> args;
	args.push_back("--background");
	args.push_back("--python-exit-code");
	args.push_back("1");
	args.push_back("--python-expr");
	args.push_back(p_python_script);

	Error err;
	String str;
	if (p_blocking) {
		int exitcode = 0;
		err = OS::get_singleton()->execute(blender_path, args, &str, &exitcode, true);
		if (exitcode != 0) {
			print_error(vformat("Blender import failed: %s.", str));
			return FAILED;
		}
	} else {
		err = OS::get_singleton()->create_process(blender_path, args, &blender_pid);
	}
	return err;
}

bool EditorImportBlendRunner::is_running() {
	return blender_pid != 0 && OS::get_singleton()->is_process_running(blender_pid);
}

Error EditorImportBlendRunner::do_import(const Dictionary &p_options) {
	if (is_using_rpc()) {
		Error err = do_import_rpc(p_options);
		if (err != OK) {
			// Retry without using RPC (slow, but better than the import failing completely).
			if (err == ERR_CONNECTION_ERROR) {
				// Disable RPC if the connection could not be established.
				print_error(vformat("Failed to connect to Blender via RPC, switching to direct imports of .blend files. Check your proxy and firewall settings, then RPC can be re-enabled by changing the editor setting `filesystem/import/blender/rpc_port` to %d.", rpc_port));
				EditorSettings::get_singleton()->set_manually("filesystem/import/blender/rpc_port", 0);
				rpc_port = 0;
			}
			if (err != ERR_QUERY_FAILED) {
				err = do_import_direct(p_options);
			}
		}
		return err;
	} else {
		return do_import_direct(p_options);
	}
}

HTTPClient::Status EditorImportBlendRunner::connect_blender_rpc(const Ref<HTTPClient> &p_client, int p_timeout_usecs) {
	p_client->connect_to_host("127.0.0.1", rpc_port);
	HTTPClient::Status status = p_client->get_status();

	int attempts = 1;
	int wait_usecs = 1000;

	bool done = false;
	while (!done) {
		OS::get_singleton()->delay_usec(wait_usecs);
		attempts++;
		status = p_client->get_status();
		switch (status) {
			case HTTPClient::STATUS_RESOLVING:
			case HTTPClient::STATUS_CONNECTING: {
				p_client->poll();
				break;
			}
			case HTTPClient::STATUS_CONNECTED: {
				done = true;
				break;
			}
			default: {
				if (attempts * wait_usecs < p_timeout_usecs) {
					p_client->connect_to_host("127.0.0.1", rpc_port);
				} else {
					return status;
				}
			}
		}
	}

	return status;
}

Error EditorImportBlendRunner::do_import_rpc(const Dictionary &p_options) {
	kill_timer->stop();

	// Start Blender if not already running.
	if (!is_running()) {
		// Start an XML RPC server on the given port.
		// The SHE selection block is appended after the RPC server loop: its
		// definitions are late-bound, `export_gltf` resolves them at call time.
		String python = vformat(PYTHON_SCRIPT_RPC, rpc_port) + PYTHON_SCRIPT_SHE_SELECT;
		Error err = start_blender(python, false);
		if (err != OK || blender_pid == 0) {
			return FAILED;
		}
	}

	// Convert options to XML body.
	String xml_options = dict_to_xmlrpc(p_options);
	String xml_body = vformat("<?xml version=\"1.0\"?><methodCall><methodName>export_gltf</methodName><params><param><value>%s</value></param></params></methodCall>", xml_options);

	// Connect to RPC server.
	Ref<HTTPClient> client = HTTPClient::create();
	HTTPClient::Status status = connect_blender_rpc(client, 1000000);
	if (status != HTTPClient::STATUS_CONNECTED) {
		ERR_FAIL_V_MSG(ERR_CONNECTION_ERROR, vformat("Unexpected status during RPC connection: %d", status));
	}

	// Send XML request.
	PackedByteArray xml_buffer = xml_body.to_utf8_buffer();
	Error err = client->request(HTTPClient::METHOD_POST, "/", Vector<String>(), xml_buffer.ptr(), xml_buffer.size());
	if (err != OK) {
		ERR_FAIL_V_MSG(err, vformat("Unable to send RPC request: %d", err));
	}

	// Wait for response.
	bool done = false;
	PackedByteArray response;
	while (!done) {
		status = client->get_status();
		switch (status) {
			case HTTPClient::STATUS_REQUESTING: {
				client->poll();
				break;
			}
			case HTTPClient::STATUS_BODY: {
				client->poll();
				response.append_array(client->read_response_body_chunk());
				break;
			}
			case HTTPClient::STATUS_CONNECTED: {
				done = true;
				break;
			}
			default: {
				ERR_FAIL_V_MSG(ERR_CONNECTION_ERROR, vformat("Unexpected status during RPC response: %d", status));
			}
		}
	}

	String response_text = "No response from Blender.";
	if (response.size() > 0) {
		response_text = String::utf8((const char *)response.ptr(), response.size());
	}

	if (client->get_response_code() != HTTPClient::RESPONSE_OK) {
		ERR_FAIL_V_MSG(ERR_QUERY_FAILED, vformat("Error received from Blender - status code: %s, error: %s", client->get_response_code(), response_text));
	} else if (response_text.find("BLENDER_GODOT_EXPORT_SUCCESSFUL") < 0) {
		// Previous versions of Godot used a Python script where the RPC function did not return
		// a value, causing the error 'cannot marshal None unless allow_none is enabled'.
		// If an older version of Godot is running and has started Blender with this script,
		// we will receive the error, but there's a good chance that the import was successful.
		// We are discarding this error to maintain backward compatibility and prevent situations
		// where the user needs to close the older version of Godot or kill Blender.
		if (response_text.find("cannot marshal None unless allow_none is enabled") < 0) {
			String error_message;
			if (_extract_error_message_xml(response, error_message)) {
				ERR_FAIL_V_MSG(ERR_QUERY_FAILED, vformat("Blender exportation failed: %s", error_message));
			} else {
				ERR_FAIL_V_MSG(ERR_QUERY_FAILED, vformat("Blender exportation failed: %s", response_text));
			}
		}
	}

	return OK;
}

bool EditorImportBlendRunner::_extract_error_message_xml(const Vector<uint8_t> &p_response_data, String &r_error_message) {
	// Based on RPC Xml spec from: https://xmlrpc.com/spec.md
	Ref<XMLParser> parser = memnew(XMLParser);
	Error err = parser->open_buffer(p_response_data);
	if (err) {
		return false;
	}

	r_error_message = String();
	while (parser->read() == OK) {
		if (parser->get_node_type() == XMLParser::NODE_TEXT) {
			if (parser->get_node_data().size()) {
				if (r_error_message.size()) {
					r_error_message += " ";
				}
				r_error_message += parser->get_node_data().trim_suffix("\n");
			}
		}
	}

	return r_error_message.size();
}

Error EditorImportBlendRunner::do_import_direct(const Dictionary &p_options) {
	// Export glTF directly. The SHE selection block is inlined through the
	// second placeholder so its definitions parse before the export flow.
	String python = vformat(PYTHON_SCRIPT_DIRECT, dict_to_python(p_options), PYTHON_SCRIPT_SHE_SELECT);
	RETURN_IF_ERROR(start_blender(python, true));

	return OK;
}

void EditorImportBlendRunner::_resources_reimported(const PackedStringArray &p_files) {
	if (is_running()) {
		// After a batch of imports is done, wait a few seconds before trying to kill blender,
		// in case of having multiple imports trigger in quick succession.
		kill_timer->start();
	}
}

void EditorImportBlendRunner::_kill_blender() {
	kill_timer->stop();
	if (is_running()) {
		OS::get_singleton()->kill(blender_pid);
	}
	blender_pid = 0;
}

void EditorImportBlendRunner::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PREDELETE: {
			_kill_blender();
			break;
		}
	}
}

EditorImportBlendRunner *EditorImportBlendRunner::singleton = nullptr;

EditorImportBlendRunner::EditorImportBlendRunner() {
	ERR_FAIL_COND_MSG(singleton != nullptr, "EditorImportBlendRunner already created.");
	singleton = this;

	rpc_port = EDITOR_GET("filesystem/import/blender/rpc_port");

	kill_timer = memnew(Timer);
	add_child(kill_timer);
	kill_timer->set_one_shot(true);
	kill_timer->set_wait_time(EDITOR_GET("filesystem/import/blender/rpc_server_uptime"));
	kill_timer->connect("timeout", callable_mp(this, &EditorImportBlendRunner::_kill_blender));

	EditorFileSystem::get_singleton()->connect("resources_reimported", callable_mp(this, &EditorImportBlendRunner::_resources_reimported));
}
