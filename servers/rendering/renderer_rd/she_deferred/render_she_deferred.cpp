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

	// Update the light buffers (directional light data + count). Forward+
	// does this in _pre_opaque_render(), which the deferred path does not run.
	{
		RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
		uint32_t directional_light_count = 0;
		uint32_t positional_light_count = 0;
		light_storage->update_light_buffers(p_render_data, *p_render_data->lights, p_render_data->scene_data->cam_transform, p_render_data->shadow_atlas, false, directional_light_count, positional_light_count, p_render_data->directional_light_soft_shadows);
		p_render_data->directional_light_count = directional_light_count;
		p_render_data->scene_data->directional_light_count = directional_light_count;
	}

	// Bake the cluster data now that every positional light has been added.
	current_cluster_builder->bake_cluster();

	p_render_data->scene_data->directional_light_count = p_render_data->directional_light_count;

	Size2i screen_size = rb->get_internal_size();

	RENDER_TIMESTAMP("SHE-deferred Geometry Pass");
	RD::get_singleton()->draw_command_begin_label("SHE-deferred Geometry Pass");

	// Geometry pass: fill the G-buffer reusing the Forward+ material-mode
	// pipelines (albedo, normal, ORM, emission, view-space depth + depth).
	_render_material(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_orthogonal, *p_render_data->instances, rb_data->get_gbuffer_fb(), Rect2i(), 1.0);

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

		// Mirror the Forward+ render pass set: bind the reflection atlas
		// texture only when it is actually valid.
		RID ref_texture = p_render_data->reflection_atlas.is_valid() ? RendererRD::LightStorage::get_singleton()->reflection_atlas_get_texture(p_render_data->reflection_atlas) : RID();

		RID u_scene_data = UniformSetCacheRD::get_singleton()->get_cache(lighting_shader_rid, 0,
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, lighting_scene_data_ubo),
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, RendererRD::LightStorage::get_singleton()->get_directional_light_buffer()),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ default_sampler, rb_data->get_albedo() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ default_sampler, rb_data->get_normal() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ default_sampler, rb_data->get_orm() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ default_sampler, rb_data->get_emission() })),
				RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ default_sampler, rb_data->get_depth_attachment() })),
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
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, current_cluster_builder->get_cluster_buffer()));

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
		} push_constant;
		push_constant.directional_light_count = p_render_data->directional_light_count;
		push_constant.luminance_multiplier = rb->get_luminance_multiplier();
		push_constant.cluster_shift = cluster_shift;
		push_constant.cluster_width = cluster_width;
		push_constant.cluster_type_size = cluster_type_size;
		push_constant.max_cluster_element_count_div_32 = max_cluster_element_count_div_32;

		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(color_fb, RD::DRAW_CLEAR_COLOR_0, Vector<Color>({ clear_color }));
		RD::get_singleton()->draw_list_bind_render_pipeline(draw_list, lighting_pipeline.get_render_pipeline(RD::INVALID_ID, RD::get_singleton()->framebuffer_get_format(color_fb)));
		RD::get_singleton()->draw_list_bind_uniform_set(draw_list, u_scene_data, 0);
		RD::get_singleton()->draw_list_bind_index_array(draw_list, RendererRD::MaterialStorage::get_singleton()->get_quad_index_array());
		RD::get_singleton()->draw_list_set_push_constant(draw_list, &push_constant, sizeof(push_constant));
		RD::get_singleton()->draw_list_draw(draw_list, true);
		RD::get_singleton()->draw_list_end();
	}

	RD::get_singleton()->draw_command_end_label();


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
