/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "she_dg_buffer.h"
#include "she_import_interpreter.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/plugins/editor_plugin.h" // EditorPlugins (hub contracts plugin registration).
#include "editor/she_hub_contracts_plugin.h"
#endif

// The SHE SDK module (chantier `sdk`, batch 2A2: marker interpretation; batch
// 2A3: DGBuffer generation at scene import). The marker and DG interpretation
// lives here and is consumed by the editor scene importer through ClassDB
// dispatch (ResourceImporterScene instantiates SHEMarkerInterpreter and
// delegates at its additive interpretation points), so the editor never
// links against module symbols. The DGBuffer resource is the camera-side
// property-name contract resolved by Camera3D (which stays module-agnostic).
// Batch 2A5 adds the hub-contracts editor plugin (play.json/catalog.json
// regeneration at EditorFileSystem scan end, editor builds only — the
// module's editor/ sources are behind the SCsub editor guard, template
// builds never include them); the UI batches route their panels the same way.

void initialize_she_sdk_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(SHEMarkerInterpreter);
		GDREGISTER_CLASS(DGBuffer);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(SHEHubContractsPlugin);
		EditorPlugins::add_by_type<SHEHubContractsPlugin>();
	}
#endif // TOOLS_ENABLED
}

void uninitialize_she_sdk_module(ModuleInitializationLevel p_level) {
}
