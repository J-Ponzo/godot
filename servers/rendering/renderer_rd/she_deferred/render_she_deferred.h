/**************************************************************************/
/*  render_she_deferred.h                                                 */
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

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/storage/ltc_lut.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/best_fit_normal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/she_deferred/she_deferred_lighting.glsl.gen.h"
#ifdef TOOLS_ENABLED
#include "servers/rendering/renderer_rd/shaders/she_deferred/she_debug_view.glsl.gen.h"
#endif // TOOLS_ENABLED
#include "servers/rendering/renderer_rd/shaders/she_deferred/she_dg_init.glsl.gen.h"
#include "servers/rendering/renderer_rd/she_deferred/render_buffer_data_she_deferred.h"

#define RB_SCOPE_SHE_DEFERRED SNAME("she_deferred")

#define RB_TEX_GB_ALBEDO SNAME("gbuffer_albedo")
#define RB_TEX_GB_NORMAL SNAME("gbuffer_normal")
#define RB_TEX_GB_ORM SNAME("gbuffer_orm")
#define RB_TEX_GB_EMISSION SNAME("gbuffer_emission")
#define RB_TEX_GB_DEPTH SNAME("gbuffer_depth")
// SHEngine fork addition (chantier sdk, batch 2A3): copy of the linear HDR
// color buffer owned by the gbuffer dump (the internal color texture cannot
// be read back directly), created lazily on the first dump. Editor builds
// only (TOOLS_ENABLED): dump instrumentation never ships in packaged game
// templates.
#ifdef TOOLS_ENABLED
#define RB_TEX_HDR_DUMP SNAME("hdr_dump_frame")
#endif // TOOLS_ENABLED

namespace RendererSceneRenderImplementation {

// SHEngine fork addition: the SHE-deferred renderer.
//
// Additive deferred renderer living next to Forward+ and Mobile. It is
// selected with the "she_deferred" rendering method and builds on the
// Forward+ renderer (storages, cluster builder, light pipeline) to keep the
// divergence from upstream minimal.
//
// Step B1: deferred for opaque geometry.
// - Geometry pass: reuses the Forward+ material-mode pipelines
//   (MODE_RENDER_MATERIAL) to fill the G-buffer (albedo, normal, ORM,
//   emission, view-space depth), orchestrated by this class with the real
//   render data (main camera projection, flip_y corrected) so the G-buffer
//   and the shared base depth buffer written alongside it are in main-pass
//   orientation, like the Forward+ passes (no vertical mirror to undo in
//   the shading pass, and a Forward+ transparent pass can depth-test
//   against that depth buffer as-is).
// - Shading pass: fullscreen pass applying ambient and directional light
//   contributions to the G-buffer, into the HDR color buffer.
// Step B2: clustered positional lights (omni/spot). The inherited Forward+
// cluster builder is driven by our _render_scene() (begin, update_light_buffers
// which feeds it, bake) and the shading pass iterates its cluster buffer with
// the shared Forward+ light processing functions.
// Step B3: shadow maps (directional PSSM, omni cube/dual paraboloid, spot).
// The shadow passes are rendered through the inherited Forward+
// _render_shadow_passes() (extracted from _pre_opaque_render for this
// purpose) and the shading pass samples the shadow atlases through the
// shared Forward+ light processing functions and the shared directional PSSM
// include (scene_forward_directional_shadow_inc.glsl), with the inherited
// Forward+ comparison sampler. Shadow filter sample counts are passed
// through the push constant, read from the shared quality getters at frame
// time (they are only valid after RendererSceneRenderRD::init()).
// Step B4: transparent objects through the inherited Forward+ color pass
// (the intentional Forward+ reuse case, see she_deferred/README.md). The
// render lists are filled with the Forward+ color-pass classification
// (opaque list -> G-buffer geometry pass, alpha list -> transparent pass):
// the transparent pass blends over the shaded opaque result (same HDR
// internal texture), depth-testing against the shared base depth buffer
// written by the geometry pass. Screen/depth backbuffer copies for materials
// sampling them run right before it, like Forward+.
// Unsupported cases (reflection probes, multiview) fail loudly instead of
// silently falling back to Forward+ (see the she_deferred/README.md).
// GI, volumetrics are not handled by the deferred path yet.
class RenderSHEDeferred : public RenderForwardClustered {
private:
	// Step B1: fullscreen deferred lighting pass.
	SheDeferredLightingShaderRD lighting_shader;
	RID lighting_shader_version;
	PipelineCacheRD lighting_pipeline;
	RID lighting_scene_data_ubo = RID();

	// Multiscatter energy compensation LUT, generated like the Forward+ one
	// (the Forward+ dfg_lut member is private, its shader class is shared).
	RID dfg_texture = RID();

	// Area light LTC lookup tables, created from the shared ltc_lut.gen.h
	// like the Forward+ ones (its ltc member is private). Created lazily in
	// ensure_lighting_luts(): RS is not ready during construction.
	RID ltc_lut1_texture = RID();
	RID ltc_lut2_texture = RID();

	// Chantier sdk, batch 2A3: fullscreen passthrough blit (debug draw
	// attachment views + the HDR copy of the gbuffer dump) and the dump
	// itself (RenderingServer::she_request_gbuffer_dump()). Editor builds
	// only (TOOLS_ENABLED): this instrumentation never ships in packaged
	// game templates (the GLSL source is not even registered).
#ifdef TOOLS_ENABLED
	SheDebugViewShaderRD debug_view_shader;
	RID debug_view_shader_version;
	PipelineCacheRD debug_view_pipeline;

	void _render_she_debug_view(const RID &p_source_rd_texture, const RID &p_dest_fb);
	void _she_dump_gbuffer(RenderDataRD *p_render_data, const Ref<RenderBufferDataSHEDeferred> &p_rb_data, const String &p_dump_dir);
#endif // TOOLS_ENABLED

	void ensure_lighting_luts();

	// Chantier sdk, batch 2A3: the DG-buffer G-buffer init pass (fullscreen
	// quad writing albedo/normal/ORM, the view-space depth attachment and
	// the base depth buffer from the camera's baked DG maps, before the
	// geometry pass - see shaders/she_deferred/she_dg_init.glsl). Returns
	// whether the G-buffer was initialized (the geometry pass then clears
	// only the emission attachment and loads everything else).
	SheDgInitShaderRD dg_init_shader;
	RID dg_init_shader_version;
	PipelineCacheRD dg_init_pipeline;
	RID dg_init_scene_data_ubo = RID();
	// Best-fit-normal LUT for the init's normal encoder, generated at
	// construction like the forward shader's (the forward member is
	// private to SceneShaderForwardClustered, same regeneration pattern as
	// the DFG LUT).
	RID best_fit_normal_texture = RID();

	bool _she_init_gbuffer_from_dg(RenderDataRD *p_render_data, const Ref<RenderBufferDataSHEDeferred> &p_rb_data, const Color &p_default_bg_color);

public:
	void setup_render_buffer_data(Ref<RenderSceneBuffersRD> p_render_buffers) override;

	void _render_scene(RenderDataRD *p_render_data, const Color &p_default_bg_color) override;

	RenderSHEDeferred();
	~RenderSHEDeferred();
};

} // namespace RendererSceneRenderImplementation
