/**************************************************************************/
/*  she_dg_buffer.cpp                                                      */
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

#include "she_dg_buffer.h"

#include "core/object/class_db.h"

void DGBuffer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &DGBuffer::set_depth);
	ClassDB::bind_method(D_METHOD("get_depth"), &DGBuffer::get_depth);
	ClassDB::bind_method(D_METHOD("set_albedo", "albedo"), &DGBuffer::set_albedo);
	ClassDB::bind_method(D_METHOD("get_albedo"), &DGBuffer::get_albedo);
	ClassDB::bind_method(D_METHOD("set_normal", "normal"), &DGBuffer::set_normal);
	ClassDB::bind_method(D_METHOD("get_normal"), &DGBuffer::get_normal);
	ClassDB::bind_method(D_METHOD("set_orm", "orm"), &DGBuffer::set_orm);
	ClassDB::bind_method(D_METHOD("get_orm"), &DGBuffer::get_orm);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "depth", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "albedo", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_albedo", "get_albedo");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "normal", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_normal", "get_normal");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "orm", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_orm", "get_orm");
}

void DGBuffer::set_depth(const Ref<Texture2D> &p_depth) {
	depth = p_depth;
	emit_changed();
}

Ref<Texture2D> DGBuffer::get_depth() const {
	return depth;
}

void DGBuffer::set_albedo(const Ref<Texture2D> &p_albedo) {
	albedo = p_albedo;
	emit_changed();
}

Ref<Texture2D> DGBuffer::get_albedo() const {
	return albedo;
}

void DGBuffer::set_normal(const Ref<Texture2D> &p_normal) {
	normal = p_normal;
	emit_changed();
}

Ref<Texture2D> DGBuffer::get_normal() const {
	return normal;
}

void DGBuffer::set_orm(const Ref<Texture2D> &p_orm) {
	orm = p_orm;
	emit_changed();
}

Ref<Texture2D> DGBuffer::get_orm() const {
	return orm;
}
