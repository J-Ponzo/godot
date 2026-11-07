/**************************************************************************/
/*  render_buffer_data_she_deferred.h                                     */
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
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                  */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererSceneRenderImplementation {

// SHEngine fork addition: custom render buffer data for the SHE-deferred
// renderer. Owns the G-buffer textures used by the deferred geometry pass.
//
// Layout (see she_deferred/README.md): the geometry pass reuses the Forward+
// material-mode pipelines (MODE_RENDER_MATERIAL):
// - "gbuffer_albedo"   RGBA16F: albedo.rgb, alpha. Widened from the bake RGBA8
//   starting point: mid-gray albedo values (e.g. 0.5/0.55, exactly between two
//   UNORM8 levels) shift by ~2/255 with 8-bit storage, which fails the cross
//   ssim threshold on broad lit surfaces (lights_many_directional_001).
// - "gbuffer_normal"   RGBA8: 24-bit encoded normal in rgb, a unused.
// - "gbuffer_orm"      RGBA8: ao (r), roughness (g), metallic (b), sss (a).
// - "gbuffer_emission" RGBA16F: emission.rgb.
// - "gbuffer_depth"    R32F: view-space linear depth (-vertex.z). Mandatory
//   5th sink attachment of the material-mode pipelines, never read back.
// - depth attachment: the shared base depth buffer (rb->get_depth_texture()),
//   written by the geometry pass under the main camera projection, in the
//   same conventions as the Forward+ passes, so the shading pass and the
//   future transparent pass can read it directly (see README.md).
class RenderBufferDataSHEDeferred : public RenderBufferCustomDataRD {
	GDCLASS(RenderBufferDataSHEDeferred, RenderBufferCustomDataRD)

private:
	RenderSceneBuffersRD *render_buffers = nullptr;

public:
	static RD::DataFormat get_gbuffer_format() {
		return RD::DATA_FORMAT_R8G8B8A8_UNORM;
	}

	static RD::DataFormat get_gbuffer_albedo_format() {
		return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	}

	static RD::DataFormat get_gbuffer_emission_format() {
		return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	}

	static RD::DataFormat get_gbuffer_depth_format() {
		return RD::DATA_FORMAT_R32_SFLOAT;
	}

	static uint32_t get_gbuffer_usage_bits(bool p_msaa, bool p_storage) {
		return RenderSceneBuffersRD::get_color_usage_bits(false, p_msaa, p_storage);
	}

	void ensure_gbuffer();
	bool has_gbuffer() const;
	void free_data() override;
	void configure(RenderSceneBuffersRD *p_render_buffers) override;

	RID get_albedo() const;
	RID get_normal() const;
	RID get_orm() const;
	RID get_emission() const;
	RID get_depth_color() const;

	// Framebuffer with the [albedo, normal, orm, emission, depth_color] color
	// attachments plus the shared base depth buffer as depth attachment,
	// used by the deferred geometry pass.
	RID get_gbuffer_fb();
};

} // namespace RendererSceneRenderImplementation
