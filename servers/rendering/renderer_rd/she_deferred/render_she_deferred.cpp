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

#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/she_deferred/render_buffer_data_she_deferred.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace RendererSceneRenderImplementation {

void RenderSHEDeferred::setup_render_buffer_data(Ref<RenderSceneBuffersRD> p_render_buffers) {
	// The inherited Forward+ machinery (used for its storages and its
	// material-mode geometry pass) needs its own custom render buffer data,
	// as does GI.
	RenderForwardClustered::setup_render_buffer_data(p_render_buffers);

	Ref<RenderBufferDataSHEDeferred> data;
	data.instantiate();
	p_render_buffers->set_custom_data(RB_SCOPE_SHE_DEFERRED, data);
}

void RenderSHEDeferred::ensure_lighting_luts() {
	if (ltc_lut1_texture.is_valid()) {
		return;
	}

	/* Area light LTC lookup tables (copy of the Forward+ initialization, see
	 * RenderForwardClustered::_update_render_base_uniform_set(); the LUT data
	 * comes from the shared storage/ltc_lut.gen.h). Created lazily: the
	 * Forward+ renderer does the same, the RS singleton is not ready during
	 * construction. */
	Ref<Image> lut1_image;
	int dimensions = LTC_LUT_DIMENSIONS;
	int lut1_bytes = 4 * dimensions * dimensions;
	size_t lut1_size = lut1_bytes * 4; // float

	Vector<uint8_t> lut1_data;
	lut1_data.resize(lut1_size);

	memcpy(lut1_data.ptrw(), LTC_LUT1, lut1_size);
	lut1_image = Image::create_from_data(dimensions, dimensions, false, Image::FORMAT_RGBAF, lut1_data);

	ltc_lut1_texture = RS::get_singleton()->texture_2d_create(lut1_image);

	int lut2_bytes = 4 * dimensions * dimensions;
	size_t lut2_size = lut2_bytes * 4; // float

	Ref<Image> lut2_image;
	Vector<uint8_t> lut2_data;
	lut2_data.resize(lut2_size);

	memcpy(lut2_data.ptrw(), LTC_LUT2, lut2_size);
	lut2_image = Image::create_from_data(dimensions, dimensions, false, Image::FORMAT_RGBAF, lut2_data);

	ltc_lut2_texture = RS::get_singleton()->texture_2d_create(lut2_image);
}

void RenderSHEDeferred::_render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) {
	// Every _setup_environment() call (the shadow passes below, the geometry
	// pass) takes a scene uniform buffer from the pool indexed by this
	// counter, so reset it at the start of the render like
	// RenderForwardClustered::_render_scene() does.
	scene_state.used_uniform_buffer_count = 0;

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	// Features the deferred path does not support are NOT silently rendered
	// through Forward+ (see she_deferred/README.md): fail loudly so the
	// missing feature is obvious, until it is implemented intentionally.
	if (p_render_data->reflection_probe.is_valid()) {
		ERR_PRINT_ONCE("SHE-deferred: reflection probes are not supported yet, skipping render.");
		return;
	}
	if (rb->get_view_count() > 1) {
		ERR_PRINT_ONCE("SHE-deferred: multiview is not supported yet, skipping render.");
		return;
	}

	Ref<RenderBufferDataSHEDeferred> rb_data;
	if (rb->has_custom_data(RB_SCOPE_SHE_DEFERRED)) {
		rb_data = rb->get_custom_data(RB_SCOPE_SHE_DEFERRED);
	}
	ERR_FAIL_NULL(rb_data);

	if (rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED) {
		WARN_PRINT_ONCE("SHE-deferred: MSAA is not supported by the deferred path yet, rendering ignoring it.");
	}

	// Positional light clusters (step B2): reuse the inherited Forward+
	// cluster builder living on the Forward+ render buffer data (created by
	// the inherited setup_render_buffer_data()). update_light_buffers(), called
	// below, feeds it through the setup_added_light() hook; bake_cluster()
	// then produces the buffer read by the lighting pass.
	Ref<RenderForwardClustered::RenderBufferDataForwardClustered> forward_rb_data;
	if (rb->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
		forward_rb_data = rb->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
	}
	ERR_FAIL_NULL(forward_rb_data);

	// A reflection probe render was rejected above, so this is always a
	// normal (non-flipped-Y) viewport render.
	current_cluster_builder = forward_rb_data->cluster_builder;
	current_cluster_builder->begin(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, true);

	// Shadow maps (step B3): render the scheduled directional/omni/spot
	// shadow passes through the inherited Forward+ orchestration
	// (_render_shadow_passes, extracted from _pre_opaque_render) BEFORE the
	// light buffers are refreshed, so the shadow state they mutate
	// (directional atlas rects, finalized omni cubemaps) is picked up. This
	// mirrors the Forward+ order: its shadow passes run between the two
	// _setup_environment() calls that update the light buffers. The deferred
	// path runs no GI, so shadow end follows immediately.
	if (p_render_data->render_shadow_count > 0) {
		if (_render_shadow_passes(p_render_data, false)) {
			_render_shadow_end();
		}
	}

	// The shadow passes internally run the inherited Forward+
	// _setup_environment() (through _render_shadow_append(), which drives the
	// cluster builder with the shadow pass data); re-begin the builder with
	// the main camera before adding the scene lights, like the Forward+
	// _setup_environment() that follows _pre_opaque_render().
	current_cluster_builder->begin(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, true);

	// Mirror the Forward+ _pre_opaque_render() cluster bookkeeping on the
	// render data: _setup_environment() (below, from the geometry pass)
	// computes cluster layout parameters from these fields like the Forward+
	// passes do.
	p_render_data->cluster_buffer = current_cluster_builder->get_cluster_buffer();
	p_render_data->cluster_size = current_cluster_builder->get_cluster_size();
	p_render_data->cluster_max_elements = current_cluster_builder->get_max_cluster_elements();

	// Update the light buffers (directional light data + count, positional
	// shadow matrices). Forward+ does this in _setup_environment(), which
	// the deferred path does not run.
	bool positional_light_soft_shadows = false;
	{
		RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
		uint32_t directional_light_count = 0;
		uint32_t positional_light_count = 0;
		light_storage->update_light_buffers(p_render_data, *p_render_data->lights, p_render_data->scene_data->cam_transform, p_render_data->shadow_atlas, true, directional_light_count, positional_light_count, p_render_data->directional_light_soft_shadows);
		p_render_data->directional_light_count = directional_light_count;
		p_render_data->scene_data->directional_light_count = directional_light_count;

		// Forward+ decides positional soft shadows per instance from its
		// material light lists (uses_softshadow: light has a non-zero size,
		// or is an area light). The deferred shading pass has no per-pixel
		// instance data, so the flag is scene-global here; the per-light
		// size check inside the shared light processing functions keeps the
		// result identical pixel by pixel.
		for (int i = 0; i < (int)p_render_data->lights->size(); i++) {
			RID base = light_storage->light_instance_get_base_light((*p_render_data->lights)[i]);
			if (light_storage->light_get_type(base) == RSE::LIGHT_DIRECTIONAL) {
				continue;
			}
			if (light_storage->light_get_type(base) == RSE::LIGHT_AREA ||
					light_storage->light_get_param(base, RSE::LIGHT_PARAM_SIZE) > CMP_EPSILON) {
				positional_light_soft_shadows = true;
				break;
			}
		}
	}

	// Decals (step B5): update the decal buffer at the exact Forward+
	// _pre_opaque_render() call site (between update_light_buffers() and
	// bake_cluster()). update_decal_buffer() stores the DecalData entries
	// and feeds the current cluster builder through the inherited
	// setup_added_decal() hook, so the baked cluster carries decal bitmasks
	// for both consumers of this frame's cluster: the deferred lighting
	// pass (decals applied to the opaque G-buffer samples) and the reused
	// Forward+ transparent pass below (whose color pipelines run the
	// upstream decal block over transparent fragments, like in Forward+).
	RendererRD::TextureStorage::get_singleton()->update_decal_buffer(*p_render_data->decals, p_render_data->scene_data->cam_transform);

	// Bake the cluster data now that every positional light has been added.
	current_cluster_builder->bake_cluster();

	Size2i screen_size = rb->get_internal_size();

	RENDER_TIMESTAMP("SHE-deferred Geometry Pass");
	RD::get_singleton()->draw_command_begin_label("SHE-deferred Geometry Pass");

	// Geometry pass: fill the G-buffer reusing the Forward+ material-mode
	// pipelines (albedo, normal, ORM, emission, view-space depth + depth).
	// It orchestrates the same inherited pieces as the upstream
	// _render_material() helper (which the deferred path used until now),
	// but with the real render data instead of that helper's own local
	// (non-flip_y-corrected) scene data: the geometry is rasterized with the
	// main camera projection, so the G-buffer and the shared base depth
	// buffer it writes are in main-pass orientation, like the Forward+
	// passes (the shading pass samples them unmirrored, and the transparent
	// pass below depth-tests against that buffer as-is).
	enable_features(FEATURE_ADVANCED_BIT);
	_update_render_base_uniform_set();

	uint32_t gbuffer_uniform_buffer_index = _setup_environment(p_render_data, false, screen_size, screen_size, p_default_bg_color, false);

	// The render lists are filled with the Forward+ color-pass
	// classification (PASS_MODE_COLOR on the opaque list): it is the only
	// fill mode routing alpha and distance-faded surfaces into the alpha
	// list (color_pass_inclusion_mask = COLOR_PASS_FLAG_TRANSPARENT), which
	// the transparent pass draws after the shading pass. Until now the fill
	// used the material pass mode (PASS_MODE_DEPTH_MATERIAL), whose upstream
	// branch also includes FLAG_PASS_ALPHA surfaces: transparent materials
	// leaked opaque garbage into the G-buffer. The geometry pass itself
	// keeps drawing material-mode (PASS_MODE_DEPTH_MATERIAL) pipelines for
	// its G-buffer outputs: only the list membership changed.
	_fill_render_list(RENDER_LIST_OPAQUE, p_render_data, PASS_MODE_COLOR);

	{
		// The G-buffer pass must draw exactly what a color pass with no
		// color_pass_flags would, the transparent pass owns the rest:
		// - Depth-writing transparent surfaces (depth_draw_always materials)
		//   land in both the opaque and the alpha lists, like in Forward+;
		//   its opaque color pass excludes them from the draw through the
		//   color_pass_inclusion_mask. The material-mode draw has no such
		//   check, so mirror it here.
		// - Texel-discard materials (alpha scissor / alpha hash:
		//   uses_alpha_clip, opaque-class, Forward+ draws them through its
		//   opaque color pass whose color pipelines discard the failing
		//   texels while writing depth). The material-mode pipelines
		//   upstream deliberately binarize the alpha instead of discarding
		//   (MODE_RENDER_MATERIAL), so drawing them here would paint their
		//   full quads into the G-buffer and the base depth (a lit dark
		//   halo where the mask is transparent). They are re-routed to the
		//   transparent pass instead: its color pipelines apply the exact
		//   same scissor/hash discard, and the surviving texels have alpha
		//   exactly 1 (the color pass forces it), so the blend composes the
		//   same value as the Forward+ opaque pass pixel by pixel. Known
		//   divergence, documented in the README: they no longer write the
		//   base depth (the transparent pipelines disable depth writes for
		//   them, like for any Forward+ transparent).
		RenderList &opaque_list = render_list[RENDER_LIST_OPAQUE];
		RenderList &alpha_list = render_list[RENDER_LIST_ALPHA];
		uint32_t opaque_list_size = opaque_list.elements.size();
		uint32_t kept_elements = 0;
		for (uint32_t i = 0; i < opaque_list_size; i++) {
			GeometryInstanceSurfaceDataCache *surf = opaque_list.elements[i];
			if (surf->color_pass_inclusion_mask != 0) {
				continue;
			}
			if ((surf->flags & GeometryInstanceSurfaceDataCache::FLAG_PASS_OPAQUE) &&
					surf->shader->uses_alpha_clip && !surf->shader->uses_alpha_antialiasing) {
				alpha_list.add_element(surf);
				continue;
			}
			opaque_list.elements[kept_elements++] = surf;
		}
		opaque_list.elements.resize(kept_elements);
	}

	render_list[RENDER_LIST_OPAQUE].sort_by_key();
	// Back-to-front by priority then depth, like the Forward+ alpha list
	// (consumed by the transparent pass below).
	render_list[RENDER_LIST_ALPHA].sort_by_reverse_depth_and_priority();

	int *render_info = p_render_data->render_info ? p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE] : (int *)nullptr;
	_fill_instance_data(RENDER_LIST_OPAQUE, render_info);
	_fill_instance_data(RENDER_LIST_ALPHA, render_info);

	// Flush the geometry pipeline compilation requests (the upstream call
	// site: after the fill, before the draws).
	_update_dirty_geometry_pipelines();

	RID rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_OPAQUE, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), gbuffer_uniform_buffer_index);

	{
		RenderListParameters render_list_params(render_list[RENDER_LIST_OPAQUE].elements.ptr(), render_list[RENDER_LIST_OPAQUE].element_info.ptr(), render_list[RENDER_LIST_OPAQUE].elements.size(), true, PASS_MODE_DEPTH_MATERIAL, 0, true, false, rp_uniform_set);
		Vector<Color> clear = {
			Color(0.0, 0.0, 0.0, 0), // Albedo.
			Color(0.0, 0.0, 0.0, 0), // Normal.
			Color(0.0, 0.0, 0.0, 0), // ORM.
			Color(0.0, 0.0, 0.0, 0), // Emission.
			Color(0.0, 0.0, 0.0, 0), // View-space depth (sink attachment).
		};
		_render_list_with_draw_list(&render_list_params, rb_data->get_gbuffer_fb(), RD::DRAW_CLEAR_ALL, clear, 0.0f, 0u, Rect2());
	}

	RD::get_singleton()->draw_command_end_label();

	// Background: sky is not supported yet (see README.md); use the clear
	// color for every background mode.
	Color clear_color;

	if (get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW) {
		clear_color = Color(0, 0, 0, 1);
	} else if (is_environment(p_render_data->environment)) {
		RSE::EnvironmentBG bg_mode = environment_get_background(p_render_data->environment);
		if (bg_mode == RSE::ENV_BG_SKY) {
			ERR_PRINT_ONCE("SHE-deferred: sky background is not supported yet, using the default clear color.");
		}

		float bg_energy_multiplier = environment_get_bg_energy_multiplier(p_render_data->environment);
		bg_energy_multiplier *= environment_get_bg_intensity(p_render_data->environment);

		if (p_render_data->camera_attributes.is_valid()) {
			bg_energy_multiplier *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}

		clear_color = (bg_mode == RSE::ENV_BG_COLOR) ? environment_get_bg_color(p_render_data->environment) : p_default_bg_color;
		clear_color = clear_color.srgb_to_linear();
		clear_color.r *= bg_energy_multiplier;
		clear_color.g *= bg_energy_multiplier;
		clear_color.b *= bg_energy_multiplier;
	} else {
		clear_color = p_default_bg_color.srgb_to_linear();
	}

	RENDER_TIMESTAMP("SHE-deferred Shading Pass");
	RD::get_singleton()->draw_command_begin_label("SHE-deferred Shading Pass");

	// Scene data UBO for the lighting pass.
	if (lighting_scene_data_ubo.is_null()) {
		lighting_scene_data_ubo = p_render_data->scene_data->create_uniform_buffer();
	}
	p_render_data->scene_data->update_ubo(lighting_scene_data_ubo, get_debug_draw_mode(), p_render_data->environment, RID(), p_render_data->camera_attributes, false, screen_size, screen_size, p_default_bg_color, rb->get_luminance_multiplier(), true, false);

	RID lighting_shader_rid = lighting_shader.version_get_shader(lighting_shader_version, 0);
	ERR_FAIL_COND(lighting_shader_rid.is_null());

	ensure_lighting_luts();

	RID color_fb = FramebufferCacheRD::get_singleton()->get_cache(rb->get_internal_texture());

	{
		RID default_sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		RID default_sampler_mipmaps = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

		// Decal atlas sampler for the lighting pass decal block (step B5):
		// same per-filter selection as the Forward+ render pass set
		// (_setup_render_pass_uniform_set), so the lighting pass samples the
		// decal atlas exactly like the reused Forward+ transparent pass
		// pipelines do.
		RID decal_sampler;
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST: {
				decal_sampler = rb->get_samplers().get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR: {
				decal_sampler = rb->get_samplers().get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS: {
				decal_sampler = rb->get_samplers().get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS: {
				decal_sampler = rb->get_samplers().get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
				decal_sampler = rb->get_samplers().get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				decal_sampler = rb->get_samplers().get_sampler(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			} break;
		}

		// Mirror the Forward+ render pass set: bind the reflection atlas
		// texture only when it is actually valid.
		RID ref_texture = p_render_data->reflection_atlas.is_valid() ? RendererRD::LightStorage::get_singleton()->reflection_atlas_get_texture(p_render_data->reflection_atlas) : RID();

		// Shadow atlases (step B3): bind the default depth texture when an
		// atlas does not exist, like the Forward+ render pass set.
		RID directional_shadow_texture = RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture();
		if (!directional_shadow_texture.is_valid()) {
			directional_shadow_texture = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		}
		RID shadow_atlas_texture;
		if (p_render_data->shadow_atlas.is_valid()) {
			shadow_atlas_texture = RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_render_data->shadow_atlas);
		}
		if (!shadow_atlas_texture.is_valid()) {
			shadow_atlas_texture = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
		}

		RID u_scene_data = UniformSetCacheRD::get_singleton()->get_cache(lighting_shader_rid, 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, lighting_scene_data_ubo),
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, RendererRD::LightStorage::get_singleton()->get_directional_light_buffer()),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ default_sampler, rb_data->get_albedo() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ default_sampler, rb_data->get_normal() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ default_sampler, rb_data->get_orm() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ default_sampler, rb_data->get_emission() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ default_sampler, rb->get_depth_texture() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ default_sampler, dfg_texture })),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, RendererRD::LightStorage::get_singleton()->get_omni_light_buffer()),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, RendererRD::LightStorage::get_singleton()->get_spot_light_buffer()),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 10, RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture_srgb()),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 11, default_sampler),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, RendererRD::LightStorage::get_singleton()->get_area_light_buffer()),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 13, default_sampler),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 14, default_sampler_mipmaps),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 15, RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(ltc_lut1_texture)),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 16, RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(ltc_lut2_texture)),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 17, RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture()),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, RendererRD::LightStorage::get_singleton()->get_reflection_probe_buffer()),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 19, RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture()),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 20, default_sampler_mipmaps),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 21, ref_texture.is_valid() ? ref_texture : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK)),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, current_cluster_builder->get_cluster_buffer()),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 23, directional_shadow_texture),
				RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 24, shadow_atlas_texture),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 25, shadow_sampler),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, RendererRD::TextureStorage::get_singleton()->get_decal_buffer()),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 27, decal_sampler));
		// Cluster iteration parameters for the shading pass, same layout math as
		// RenderForwardClustered::_setup_environment().
		uint32_t cluster_shift = Math::get_shift_from_power_of_2(current_cluster_builder->get_cluster_size());
		uint32_t max_cluster_element_count_div_32 = current_cluster_builder->get_max_cluster_elements() / 32;
		uint32_t cluster_width = 0;
		uint32_t cluster_type_size = 0;
		{
			uint32_t cluster_screen_width = Math::division_round_up((uint32_t)screen_size.width, current_cluster_builder->get_cluster_size());
			uint32_t cluster_screen_height = Math::division_round_up((uint32_t)screen_size.height, current_cluster_builder->get_cluster_size());
			cluster_type_size = cluster_screen_width * cluster_screen_height * (max_cluster_element_count_div_32 + 32);
			cluster_width = cluster_screen_width;
		}

		struct {
			uint32_t directional_light_count;
			float luminance_multiplier;
			uint32_t cluster_shift;
			uint32_t cluster_width;
			uint32_t cluster_type_size;
			uint32_t max_cluster_element_count_div_32;
			uint32_t feature_flags;
			uint32_t soft_shadow_samples;
			uint32_t penumbra_shadow_samples;
			uint32_t directional_soft_shadow_samples;
			uint32_t directional_penumbra_shadow_samples;
		} push_constant;
		push_constant.directional_light_count = p_render_data->directional_light_count;
		push_constant.luminance_multiplier = rb->get_luminance_multiplier();
		push_constant.cluster_shift = cluster_shift;
		push_constant.cluster_width = cluster_width;
		push_constant.cluster_type_size = cluster_type_size;
		push_constant.max_cluster_element_count_div_32 = max_cluster_element_count_div_32;
		push_constant.feature_flags = 0;
		if (p_render_data->directional_light_soft_shadows) {
			push_constant.feature_flags |= 1;
		}
		if (positional_light_soft_shadows) {
			push_constant.feature_flags |= 2;
		}
		if (GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/multi_bounce_occlusion/enabled")) {
			push_constant.feature_flags |= 4;
		}
		/* Decal atlas mipmapped filtering mirrors the Forward+ pipeline
		 * specialization (SceneShaderForwardClustered decal_use_mipmaps):
		 * the filter setting is read at frame time like the sample counts
		 * below, so a mid-session quality change does not require a
		 * pipeline rebuild here (the lighting pass is a single pipeline). */
		switch (decals_get_filter()) {
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS:
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS:
			case RSE::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC:
			case RSE::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
				push_constant.feature_flags |= 8;
			} break;
			default:
				break;
		}
		/* Shadow filter sample counts, matching the Forward+ pipeline
		 * specialization (its default_specialization is refreshed when the
		 * quality settings change). Read at frame time: they are only valid
		 * after RendererSceneRenderRD::init() applies the project settings.
		 * Not a specialization constant here: the lighting pass is a single
		 * pipeline, the counts go through the push constant instead. */
		push_constant.soft_shadow_samples = soft_shadow_samples_get();
		push_constant.penumbra_shadow_samples = penumbra_shadow_samples_get();
		push_constant.directional_soft_shadow_samples = directional_soft_shadow_samples_get();
		push_constant.directional_penumbra_shadow_samples = directional_penumbra_shadow_samples_get();

		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(color_fb, RD::DRAW_CLEAR_COLOR_0, Vector<Color>({ clear_color }));
		RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, lighting_pipeline.get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(color_fb)));
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, u_scene_data, 0);
		RD::get_singleton()->draw_list_bind_index_array(draw_list, RendererRD::MaterialStorage::get_singleton()->get_quad_index_array());
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(push_constant));
		RD::get_singleton()->draw_list_draw(draw_list, true);
		RD::get_singleton()->draw_list_end();
	}

	RD::get_singleton()->draw_command_end_label();

	// Transparent objects (step B4): the alpha render list filled by the
	// geometry pass is drawn by the inherited Forward+ color pass
	// (pipelines, framebuffer, uniform sets), blended over the shaded
	// opaque result of the lighting pass (same HDR internal texture) and
	// depth-tested against the shared base depth buffer written by the
	// geometry pass (same texture, load not clear). This is the
	// intentional Forward+ reuse case of the deferred renderer design
	// (see she_deferred/README.md): a true deferred transparent pass
	// would need a G-buffer read-light path per-pixel.

	if (scene_state.used_screen_texture || global_surface_data.screen_texture_used) {
		RENDER_TIMESTAMP("SHE-deferred Copy Screen Texture");

		// Transparent materials sampling the screen texture (refraction)
		// must see the shaded opaque scene, like the Forward+ copy that
		// runs right before its transparent pass.
		_render_buffers_ensure_screen_texture(p_render_data);

		if (scene_state.used_screen_texture) {
			_render_buffers_copy_screen_texture(p_render_data);
		}
	}

	if (scene_state.used_depth_texture || global_surface_data.depth_texture_used) {
		RENDER_TIMESTAMP("SHE-deferred Copy Depth Texture");

		_render_buffers_ensure_depth_texture(p_render_data);

		if (scene_state.used_depth_texture) {
			_render_buffers_copy_depth_texture(p_render_data);
		}
	}

	if (!render_list[RENDER_LIST_ALPHA].elements.is_empty()) {
		RENDER_TIMESTAMP("SHE-deferred Transparent Pass");
		RD::get_singleton()->draw_command_begin_label("SHE-deferred Transparent Pass");

		// Fresh scene data UBO for the transparent pass, like the Forward+
		// one (its color-pass uniforms: ambient, fog, exposure...).
		uint32_t transparent_pass_uniform_buffer_index = _setup_environment(p_render_data, false, screen_size, screen_size, p_default_bg_color, false);

		// Pipeline specialization like the Forward+ passes: quality
		// settings plus depth fog and the area light count of the baked
		// cluster.
		SceneShaderForwardClustered::ShaderSpecialization base_specialization = scene_shader.default_specialization;
		base_specialization.use_depth_fog = p_render_data->environment.is_valid() && environment_get_fog_mode(p_render_data->environment) == RSE::EnvironmentFogMode::ENV_FOG_MODE_DEPTH;
		base_specialization.cluster_has_area_light = current_cluster_builder->get_cluster_count_by_type(ClusterBuilderRD::ELEMENT_TYPE_AREA_LIGHT) != 0;

		bool reverse_cull = p_render_data->scene_data->cam_transform.basis.determinant() < 0.0;
		RendererRD::MaterialStorage::Samplers samplers = rb->get_samplers();

		rp_uniform_set = _setup_render_pass_uniform_set(RENDER_LIST_ALPHA, p_render_data, false, RID(), samplers, transparent_pass_uniform_buffer_index, true);

		{
			// The Forward+ transparent color pass as-is: color = the HDR
			// internal texture the shading pass wrote, depth = the shared
			// base depth buffer of the geometry pass (COLOR_PASS_FLAG_TRANSPARENT
			// pipelines write depth only for depth_draw_always materials,
			// like Forward+).
			RID alpha_framebuffer = forward_rb_data->get_color_pass_fb(COLOR_PASS_FLAG_TRANSPARENT);
			RenderListParameters render_list_params(render_list[RENDER_LIST_ALPHA].elements.ptr(), render_list[RENDER_LIST_ALPHA].element_info.ptr(), render_list[RENDER_LIST_ALPHA].elements.size(), reverse_cull, PASS_MODE_COLOR, COLOR_PASS_FLAG_TRANSPARENT, false, p_render_data->directional_light_soft_shadows, rp_uniform_set, get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME, Vector2(), p_render_data->scene_data->lod_distance_multiplier, p_render_data->scene_data->screen_mesh_lod_threshold, p_render_data->scene_data->view_count, 0, base_specialization, true);
			_render_list_with_draw_list(&render_list_params, alpha_framebuffer, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 0.0f, 0u, p_render_data->render_region);
		}

		RD::get_singleton()->draw_command_end_label();
	}

	RENDER_TIMESTAMP("SHE-deferred Post-Process");
	_render_buffers_post_process_and_tonemap(p_render_data);
}

RenderSHEDeferred::RenderSHEDeferred() {
	// Initialize the deferred lighting pass (step B1).
	Vector<String> lighting_modes;
	lighting_modes.push_back("\n");

	String general_defines = "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(RendererSceneRender::MAX_DIRECTIONAL_LIGHTS) + "\n";

	lighting_shader.initialize(lighting_modes, general_defines);
	lighting_shader_version = lighting_shader.version_create();

	lighting_pipeline.setup(lighting_shader.version_get_shader(lighting_shader_version, 0), RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), RD::PipelineColorBlendState::create_disabled(), 0);

	/* DFG LUT (copy of the Forward+ initialization, see
	 * RenderForwardClustered::RenderForwardClustered()). */
	{
		IntegrateDfgShaderRD dfg_shader;
		Vector<String> modes;
		modes.push_back("\n");
		dfg_shader.initialize(modes);
		RID dfg_shader_version = dfg_shader.version_create();
		RID dfg_pipeline = RD::get_singleton()->compute_pipeline_create(dfg_shader.version_get_shader(dfg_shader_version, 0));

		RD::TextureFormat tformat;
		tformat.format = RD::DATA_FORMAT_R16G16_SFLOAT;
		tformat.width = 128;
		tformat.height = 128;
		tformat.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		tformat.texture_type = RD::TEXTURE_TYPE_2D;
		dfg_texture = RD::get_singleton()->texture_create(tformat, RD::TextureView());

		RID shader = dfg_shader.version_get_shader(dfg_shader_version, 0);
		ERR_FAIL_COND(shader.is_null());

		Vector<RD::Uniform> uniforms;

		{
			RD::Uniform u;
			u.binding = 0;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dfg_texture);
			uniforms.push_back(u);
		}
		RID uniform_set = RD::get_singleton()->uniform_set_create(uniforms, shader, 0);

		RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, dfg_pipeline);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, tformat.width, tformat.height, 1);
		RD::get_singleton()->compute_list_end();

		dfg_shader.version_free(dfg_shader_version);

	}
}

RenderSHEDeferred::~RenderSHEDeferred() {
	lighting_shader.version_free(lighting_shader_version);

	// Free the deferred pass resources created with RD directly (the ones
	// owned by RS, the LTC LUTs, are freed below with RS::free_rid).
	if (dfg_texture.is_valid()) {
		RD::get_singleton()->free_rid(dfg_texture);
	}
	if (lighting_scene_data_ubo.is_valid()) {
		RD::get_singleton()->free_rid(lighting_scene_data_ubo);
	}

	if (ltc_lut1_texture.is_valid()) {
		RS::get_singleton()->free_rid(ltc_lut1_texture);
	}
	if (ltc_lut2_texture.is_valid()) {
		RS::get_singleton()->free_rid(ltc_lut2_texture);
	}
}

} // namespace RendererSceneRenderImplementation
