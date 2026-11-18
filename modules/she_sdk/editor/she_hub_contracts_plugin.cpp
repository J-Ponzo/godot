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

#include "core/object/callable_mp.h"
#include "editor/file_system/editor_file_system.h"

void SHEHubContractsPlugin::_regenerate() {
	she_sdk::regenerate_play_catalog();
}

void SHEHubContractsPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
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
