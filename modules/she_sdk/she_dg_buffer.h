/**************************************************************************/
/*  she_dg_buffer.h                                                        */
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

#pragma once

#include "core/io/resource.h"
#include "scene/resources/texture.h"

// The per-camera deterministic-layer bundle (chantier `sdk`, batch 2A3 J3).
// The property names ARE the engine-side contract: `Camera3D.dg_buffer`
// resolves them through `Object::get()` with zero knowledge of this class
// (the 2A2 ClassDB discipline - any resource exposing these names feeds
// the init pass; the J2 validation proved the chain with a throwaway
// GDScript resource before this class existed). The class carries map
// slots only - no near/far duplication: the imported Camera3D is the
// authority, and the bake/runtime clip coherence is validated at import
// (she_dg.cpp), not carried in the resource.
class DGBuffer : public Resource {
	GDCLASS(DGBuffer, Resource);

private:
	Ref<Texture2D> depth;
	Ref<Texture2D> albedo;
	Ref<Texture2D> normal;
	Ref<Texture2D> orm;

protected:
	static void _bind_methods();

public:
	void set_depth(const Ref<Texture2D> &p_depth);
	Ref<Texture2D> get_depth() const;

	void set_albedo(const Ref<Texture2D> &p_albedo);
	Ref<Texture2D> get_albedo() const;

	void set_normal(const Ref<Texture2D> &p_normal);
	Ref<Texture2D> get_normal() const;

	void set_orm(const Ref<Texture2D> &p_orm);
	Ref<Texture2D> get_orm() const;
};
