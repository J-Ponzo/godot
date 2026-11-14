/* SHEngine fork addition: SHE-deferred G-buffer init pass (chantier sdk,
 * batch 2A3).
 *
 * When a DGBuffer is bound to the active camera, this fullscreen pass
 * initializes the G-buffer from the baked deterministic-layer maps before
 * the geometry pass (the background then never rasterizes at runtime - it
 * lives in textures, and the baked depth written into the shared base
 * depth buffer occludes the interactive world for free through the depth
 * test).
 *
 * Map semantics (the bake conventions, docs/sdk/design.md; recipe source
 * of truth she-editor/she_bake.py):
 * - depth encodes the DG clip shell remap (Z - near) / (far - near), the
 *   camera clip being the shell (acté 2A1). Decoded back to a view-space
 *   distance, it feeds both the view-space depth attachment and the base
 *   depth buffer through the very projection matrix the rasterizer uses
 *   (the scene data UBO), so it is bit-consistent with every consumer of
 *   the main-pass depth by construction.
 * - normal is raw Blender WORLD-space (the Cycles Normal pass routed
 *   verbatim through the bake - proven by the sdk harness's dg oracle:
 *   the baked map reads (0,0,1) on the floor and (1,0,0) on the left
 *   wall, the Blender Z-up world basis). The runtime permutation
 *   (x, z, -y) brings it to Godot world space (POC convention, acté
 *   design.md), and the scene data UBO's view rotation brings it to the
 *   view space the G-buffer attachment stores (the material pipelines'
 *   own world->view transform upstream): the rotation is the identity
 *   for an identity-rotation camera ONLY - the POC consumers and every
 *   twin camera up to the J4 oracle were identity-aligned (view == world),
 *   which hid the missing rotation. The stored bytes then go through the
 *   same Crytek best-fit encoder as the material pipelines (encode24,
 *   copied verbatim from scene_forward_clustered.glsl where it lives
 *   behind a MODE define, with the same 1024x1024 R8 best-fit LUT the
 *   renderer generates at startup): the init and the rasterized twin are
 *   byte-comparable.
 */

#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec2 uv_interp;

void main() {
	vec2 base_arr[4] = vec2[](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
	uv_interp = base_arr[gl_VertexIndex];
	vec2 vpos = uv_interp;
	gl_Position = vec4(vpos * 2.0 - 1.0, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

#define MAX_VIEWS 2

#include "../scene_data_inc.glsl"

layout(location = 0) in vec2 uv_interp;

layout(set = 0, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
} scene_data_block;

/* The baked DG maps. The depth map must be point-sampled (the bake shell is
 * a per-pixel depth contract; interpolated depth has no meaning), the color
 * maps linearly - at equal resolution (the harness contract) both are
 * exact. */
layout(set = 0, binding = 1) uniform sampler2D dg_depth;
layout(set = 0, binding = 2) uniform sampler2D dg_albedo;
layout(set = 0, binding = 3) uniform sampler2D dg_normal;
layout(set = 0, binding = 4) uniform sampler2D dg_orm;
layout(set = 0, binding = 5) uniform sampler2D best_fit_normal_texture;

layout(location = 0) out vec4 gbuffer_albedo_io;
layout(location = 1) out vec4 gbuffer_normal_io;
layout(location = 2) out vec4 gbuffer_orm_io;
layout(location = 3) out vec4 gbuffer_depth_io;

/* Crytek best-fit normal quantization, verbatim from the shared material
 * pipelines (scene_forward_clustered.glsl, MODE_RENDER_MATERIAL), so the
 * init writes exactly the bytes the rasterizer would.
 */
vec3 encode24(vec3 v) {
	// Unsigned normal (handles most symmetry)
	vec3 vNormalUns = abs(v);
	// Get the major axis for our collapsed cubemap lookup
	float maxNAbs = max(vNormalUns.z, max(vNormalUns.x, vNormalUns.y));
	// Get the collapsed cubemap texture coordinates
	vec2 vTexCoord = vNormalUns.z < maxNAbs ? (vNormalUns.y < maxNAbs ? vNormalUns.yz : vNormalUns.xz) : vNormalUns.xy;
	vTexCoord /= maxNAbs;
	vTexCoord = vTexCoord.x < vTexCoord.y ? vTexCoord.yx : vTexCoord.xy;
	// Stretch:
	vTexCoord.y /= vTexCoord.x;
	float fFittingScale = texture(best_fit_normal_texture, vTexCoord).r;
	// Make vector touch unit cube
	vec3 result = v / maxNAbs;
	// scale the normal to get the best fit
	result *= fFittingScale;
	return result;
}

void main() {
	/* Depth: decode the clip-shell remap back to a view-space distance,
	 * then evaluate the exact device depth through the projection matrix
	 * (the geometry pass rasterizes with this very matrix, so a surface
	 * at this distance would write exactly this value; the lighting pass
	 * reconstructs through inv_projection and round-trips). The view-space
	 * depth attachment stores the distance itself. */
	float shell01 = texture(dg_depth, uv_interp).r;
	float view_z = scene_data_block.data.z_near + shell01 * (scene_data_block.data.z_far - scene_data_block.data.z_near);

	vec4 clip_position = scene_data_block.data.projection_matrix * vec4(0.0, 0.0, -view_z, 1.0);
	gl_FragDepth = clip_position.z / clip_position.w;

	/* Albedo: the baked map is linear scene color; the opaque alpha is 1
	 * like the material pipelines binarize it for opaque surfaces. */
	gbuffer_albedo_io = vec4(texture(dg_albedo, uv_interp).rgb, 1.0);

	/* Normal: Blender world -> Godot world (the Y-up axis conversion,
	 * POC convention), then the camera's view rotation into the view
	 * space the attachment stores (the material pipelines use the same
	 * read_view_matrix reconstruction upstream - w = 0 keeps it rotation
	 * only). Degenerate baked normals (no geometry in the map at this
	 * pixel, e.g. an open shell) match the G-buffer clear instead of
	 * NaN-ing the byte storage. */
	vec3 normal_blender = texture(dg_normal, uv_interp).rgb;
	vec3 normal_world = vec3(normal_blender.x, normal_blender.z, -normal_blender.y);
	if (dot(normal_world, normal_world) < 1e-8) {
		gbuffer_normal_io = vec4(0.0);
	} else {
		mat4 read_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0],
				scene_data_block.data.view_matrix[1],
				scene_data_block.data.view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));
		vec3 normal_view = normalize((read_view_matrix * vec4(normal_world, 0.0)).xyz);
		gbuffer_normal_io = vec4(encode24(normal_view) * 0.5 + 0.5, 0.0);
	}

	/* ORM: the baked map carries the Cycles-combined AO in R (material AO in
	 * r on the raster side - the SDK oracle excludes this channel by
	 * convention, docs/sdk/roadmap.md 2A3), roughness in G, metallic in B;
	 * the sss channel (a) stays clear like the rasterized opaques. */
	gbuffer_orm_io = vec4(texture(dg_orm, uv_interp).rgb, 0.0);

	gbuffer_depth_io = vec4(view_z, 0.0, 0.0, 0.0);
}
