/**************************************************************************/
/*  she_hub_contracts.h                                                    */
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

// Hub contracts generation (chantier `sdk`, batch 2A5) — the additive
// producer of `res://play.json` and `res://ingredients/catalog.json`,
// consumed headless by the hub tool and by the `she-editor` Play picker.
//
// Doctrine (mvp.md §7): contracts are tool-to-tool documents with an
// explicit schema, ALWAYS produced by their designated producer and
// NEVER edited by hand — the hub (2A5) and the Blender add-on only
// trigger or read them. Both files are regenerated as a PURE FUNCTION of
// the engine's own state (the `.blend.import` remap records of the
// `levels/<id>/` tree), after every filesystem scan completes: the
// regen runs as a no-op when the derived content is unchanged, so
// scans without SHE-relevant changes cost a few file reads.
//
// GATE: a project is only ever written when it pins the SHE renderer
// (`rendering/renderer/rendering_method == "she_deferred"`) AND
// carries a `res://levels/` directory — any other Godot project the
// binary opens stays untouched, editors and templates included.
//
// play.json (schema `she-play/1`): one automatic entry per IMPORTED
// level — `{"name": <level id>, "scene": <remap destination>,
// "origin": "level"}` — sorted by level id for byte-stable output.
// The "user" origin (programmer entries through the module panel)
// arrives with the gameplay-ingredient batches; the file stays
// integrally generated either way.
//
// catalog.json (schema `she-catalog/1`): 2A5 writes the PLACEHOLDER
// only (schema + empty `ingredients`) — the producer and the path are
// fixed here, the reflection over `res://ingredients/` scenes is
// Phase C and replaces nothing below.
//
// Diagnostics carry the `[SHE-PLAY]` / `[SHE-CATALOG]` prefixes
// (assertable by the harness and the hub probes in import logs).

#include "core/string/ustring.h"

namespace she_sdk {

// Regenerates both hub contract documents when their derived content
// differs from what is on disk (the caller decides the trigger — the
// registered plugin binds it to the EditorFileSystem scan-completion
// signal). Silent no-op outside SHE projects.
void regenerate_play_catalog();

} // namespace she_sdk
