/**************************************************************************/
/*  render_she_deferred.cpp                                               */
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

#include "render_she_deferred.h"

#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"

namespace RendererSceneRenderImplementation {

void RenderSHEDeferred::render_scene(const Ref<RenderSceneBuffers> &p_render_buffers, const CameraData *p_camera_data, const CameraData *p_prev_camera_data, const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const PagedArray<RID> &p_reflection_probes, const PagedArray<RID> &p_voxel_gi_instances, const PagedArray<RID> &p_decals, const PagedArray<RID> &p_lightmaps, const PagedArray<RID> &p_fog_volumes, RID p_environment, RID p_camera_attributes, RID p_compositor, RID p_shadow_atlas, RID p_occluder_debug_tex, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass, float p_screen_mesh_lod_threshold, const RenderShadowData *p_render_shadows, int p_render_shadow_count, const RenderSDFGIData *p_render_sdfgi_regions, int p_render_sdfgi_region_count, float p_window_output_max_value, const RenderSDFGIUpdateData *p_sdfgi_update_data, RenderingServerTypes::RenderInfo *r_render_info) {
	// Step A: render through the inherited Forward+ path...
	RenderForwardClustered::render_scene(p_render_buffers, p_camera_data, p_prev_camera_data, p_instances, p_lights, p_reflection_probes, p_voxel_gi_instances, p_decals, p_lightmaps, p_fog_volumes, p_environment, p_camera_attributes, p_compositor, p_shadow_atlas, p_occluder_debug_tex, p_reflection_atlas, p_reflection_probe, p_reflection_probe_pass, p_screen_mesh_lod_threshold, p_render_shadows, p_render_shadow_count, p_render_sdfgi_regions, p_render_sdfgi_region_count, p_window_output_max_value, p_sdfgi_update_data, r_render_info);

	// ...then paint the final render target magenta, so any captured image
	// proves this renderer (and not Forward+) was selected. The deferred
	// G-buffer and shading passes replace this in step B.
	RenderSceneBuffersRD *render_buffers = Object::cast_to<RenderSceneBuffersRD>(p_render_buffers.ptr());
	ERR_FAIL_NULL(render_buffers);

	RID render_target = render_buffers->get_render_target();
	ERR_FAIL_COND(!render_target.is_valid());

	RID render_target_framebuffer = RendererRD::TextureStorage::get_singleton()->render_target_get_rd_framebuffer(render_target);
	ERR_FAIL_COND(!render_target_framebuffer.is_valid());

	// Magenta components are 0 or 1, so the value is identical whether the
	// render target is sRGB-encoded or linear (HDR 2D).
	RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(render_target_framebuffer, RD::DRAW_CLEAR_COLOR_0, Vector<Color>({ Color(1.0f, 0.0f, 1.0f) }));
	CRASH_COND(draw_list == RD::INVALID_ID);
	RD::get_singleton()->draw_list_end();
}

} // namespace RendererSceneRenderImplementation
