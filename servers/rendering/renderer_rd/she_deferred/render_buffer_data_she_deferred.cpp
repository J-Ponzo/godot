/**************************************************************************/
/*  render_buffer_data_she_deferred.cpp                                   */
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

#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/she_deferred/render_buffer_data_she_deferred.h"
#include "servers/rendering/renderer_rd/she_deferred/render_she_deferred.h"

namespace RendererSceneRenderImplementation {

void RenderBufferDataSHEDeferred::ensure_gbuffer() {
	ERR_FAIL_NULL(render_buffers);

	if (!has_gbuffer()) {
		bool msaa = render_buffers->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
		render_buffers->create_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_ALBEDO, get_gbuffer_albedo_format(), get_gbuffer_usage_bits(msaa, render_buffers->get_can_be_storage()));
		render_buffers->create_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_NORMAL, get_gbuffer_format(), get_gbuffer_usage_bits(msaa, render_buffers->get_can_be_storage()));
		render_buffers->create_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_ORM, get_gbuffer_format(), get_gbuffer_usage_bits(msaa, render_buffers->get_can_be_storage()));
		render_buffers->create_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_EMISSION, get_gbuffer_emission_format(), get_gbuffer_usage_bits(msaa, render_buffers->get_can_be_storage()));
		render_buffers->create_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_DEPTH, get_gbuffer_depth_format(), get_gbuffer_usage_bits(msaa, render_buffers->get_can_be_storage()));
		render_buffers->create_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_DEPTH_ATTACHMENT, RenderSceneBuffersRD::get_depth_format(false, msaa, render_buffers->get_can_be_storage()), RenderSceneBuffersRD::get_depth_usage_bits(false, msaa, render_buffers->get_can_be_storage()) | RD::TEXTURE_USAGE_SAMPLING_BIT);
	}
}

bool RenderBufferDataSHEDeferred::has_gbuffer() const {
	return render_buffers && render_buffers->has_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_ALBEDO);
}

void RenderBufferDataSHEDeferred::free_data() {
	if (render_buffers) {
		render_buffers->clear_context(RB_SCOPE_SHE_DEFERRED);
	}
}

void RenderBufferDataSHEDeferred::configure(RenderSceneBuffersRD *p_render_buffers) {
	if (render_buffers) {
		// JIC
		free_data();
	}

	render_buffers = p_render_buffers;
	ERR_FAIL_NULL(render_buffers);

	// Allocate the G-buffer eagerly: the deferred passes always need it.
	ensure_gbuffer();
}

RID RenderBufferDataSHEDeferred::get_albedo() const {
	ERR_FAIL_COND_V(!render_buffers, RID());
	return render_buffers->get_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_ALBEDO);
}

RID RenderBufferDataSHEDeferred::get_normal() const {
	ERR_FAIL_COND_V(!render_buffers, RID());
	return render_buffers->get_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_NORMAL);
}

RID RenderBufferDataSHEDeferred::get_orm() const {
	ERR_FAIL_COND_V(!render_buffers, RID());
	return render_buffers->get_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_ORM);
}

RID RenderBufferDataSHEDeferred::get_emission() const {
	ERR_FAIL_COND_V(!render_buffers, RID());
	return render_buffers->get_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_EMISSION);
}

RID RenderBufferDataSHEDeferred::get_depth_color() const {
	ERR_FAIL_COND_V(!render_buffers, RID());
	return render_buffers->get_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_DEPTH);
}

RID RenderBufferDataSHEDeferred::get_depth_attachment() const {
	ERR_FAIL_COND_V(!render_buffers, RID());
	return render_buffers->get_texture(RB_SCOPE_SHE_DEFERRED, RB_TEX_GB_DEPTH_ATTACHMENT);
}

RID RenderBufferDataSHEDeferred::get_gbuffer_fb() {
	ERR_FAIL_NULL_V(render_buffers, RID());
	ensure_gbuffer();

	// The deferred geometry pass uses its own depth attachment: it renders
	// through the inherited Forward+ material pass, whose projection is not
	// flip_y-corrected like the main pass, so the shared base depth buffer
	// must not be written.
	return FramebufferCacheRD::get_singleton()->get_cache_multiview(render_buffers->get_view_count(), get_albedo(), get_normal(), get_orm(), get_emission(), get_depth_color(), get_depth_attachment());
}

} // namespace RendererSceneRenderImplementation
