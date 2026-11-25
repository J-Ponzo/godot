/**************************************************************************/
/*  she_hub_contracts_plugin.cpp                                           */
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

#include "she_hub_contracts_plugin.h"

#include "she_hub_contracts.h"

#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"

// The packaged Blender's importer pin (arbitré 2026-11-25, séance
// packaging 3A3-suite — a user session in the real package found the
// hole): modules/gltf's EditorSceneFormatImporterBlend refuses to
// import '.blend' scenes until editor setting
// `filesystem/import/blender/blender_path` points at an existing
// binary, and it resolves a RELATIVE value against the process CWD
// (FileAccess::exists) — a value no launch mode can rely on (the
// headless reimport runs from the project dir, a double-clicked editor
// from wherever the desktop feels). The package places the pinned
// Blender as a SIBLING of the editor's own directory, so the pin is
// re-derived from the executable's ABSOLUTE location at every plugin
// boot (a moved package self-heals; a DEV editor has no sibling
// Blender and stays exactly as upstream — manual setting, dialogs).
// A working setting is NEVER overwritten: an explicit user choice or
// a dev machine keeps its own Blender.
static void _pin_packaged_blender_importer() {
	static constexpr const char *SHE_SDK_PREFIX = "[SHE-SDK] ";
	const String setting_path = "filesystem/import/blender/blender_path";

	EditorSettings *settings = EditorSettings::get_singleton();
	if (settings == nullptr) {
		return; // defensive: plugin-less sessions (never expected here)
	}
	String current;
	if (settings->has_setting(setting_path)) {
		current = settings->get_setting(setting_path);
	}
	if (!current.is_empty() && FileAccess::exists(current)) {
		return; // a valid setting (user's or dev's) is never touched
	}
	const String editor_dir = OS::get_singleton()->get_executable_path().get_base_dir();
#ifdef WINDOWS_ENABLED
	static const char *CANDIDATE = "../blender/blender.exe";
#else
	static const char *CANDIDATE = "../blender/blender";
#endif
	const String candidate = editor_dir.path_join(CANDIDATE).simplify_path();
	if (!FileAccess::exists(candidate)) {
		return; // no sibling Blender (a dev editor): upstream behavior
	}
	settings->set_setting(setting_path, candidate);
	EditorSettings::save();
	print_line(vformat("%sPinned the blend importer to the packaged Blender (%s).",
			SHE_SDK_PREFIX, candidate));
}

void SHEHubContractsPlugin::_regenerate() {
	she_sdk::regenerate_play_catalog();
}

void SHEHubContractsPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// The blend importer pin comes FIRST: it must sit in the
			// editor settings before the pending EditorFileSystem scan
			// imports any '.blend' (measured: "Initializing plugins"
			// steps before "Starting file scan" in every boot, the
			// headless `--import` passes included).
			_pin_packaged_blender_importer();
			// The EditorFileSystem outlives plugin additions (the editor
			// constructs it before instantiating module plugins).
			EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
			if (filesystem) {
				Callable regenerate = callable_mp(this, &SHEHubContractsPlugin::_regenerate);
				filesystem->connect(SNAME("filesystem_changed"), regenerate);
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
			if (filesystem) {
				Callable regenerate = callable_mp(this, &SHEHubContractsPlugin::_regenerate);
				if (filesystem->is_connected(SNAME("filesystem_changed"), regenerate)) {
					filesystem->disconnect(SNAME("filesystem_changed"), regenerate);
				}
			}
		} break;
	}
}

String SHEHubContractsPlugin::get_plugin_name() const {
	return "SHEngine";
}

SHEHubContractsPlugin::SHEHubContractsPlugin() {
}
