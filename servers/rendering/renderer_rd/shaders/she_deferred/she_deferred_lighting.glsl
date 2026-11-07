/* SHEngine fork addition: SHE-deferred fullscreen lighting pass (step B1).
 *
 * Reads the G-buffer filled by the deferred geometry pass (which reuses the
 * Forward+ material-mode pipelines, see render_she_deferred.cpp) and applies
 * ambient, directional light and clustered positional light (omni/spot, step
 * B2) contributions into the HDR color buffer. Shadow maps and GI are not
 * implemented yet.
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

#include "../half_inc.glsl"

#define MAX_VIEWS 2

#include "../scene_data_inc.glsl"
#include "../light_data_inc.glsl"

#define M_PI 3.14159265359
#define MAX_ROUGHNESS_LOD 5 /* same value as the Forward+ scene shader */
#define REFLECTION_MULTIPLIER 1.0 /* same value as the Forward+ scene shader */

#include "../oct_inc.glsl"

/* Declarations required by the shared Forward+ light processing functions
 * (positional light buffers and decal atlas are shared storages; the sc_*
 * helpers are stubbed until soft shadows and light projectors land). */
layout(set = 0, binding = 8, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 9, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 12, std430) restrict readonly buffer AreaLights {
	LightData data[];
}
area_lights;

layout(set = 0, binding = 10) uniform texture2D decal_atlas_srgb;
layout(set = 0, binding = 11) uniform sampler light_projector_sampler;
layout(set = 0, binding = 13) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 0, binding = 14) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 0, binding = 15) uniform texture2D ltc_lut1;
layout(set = 0, binding = 16) uniform texture2D ltc_lut2;
layout(set = 0, binding = 17) uniform texture2D area_light_atlas;
layout(set = 0, binding = 18, std430) restrict readonly buffer ReflectionProbeData {
	ReflectionData data[];
}
reflections;
layout(set = 0, binding = 19) uniform texture2D decal_atlas;
layout(set = 0, binding = 20) uniform sampler DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 0, binding = 21) uniform texture2DArray reflection_atlas;

/* Cluster data for the positional lights, baked every frame by the
 * inherited Forward+ cluster builder (see render_she_deferred.cpp). */
layout(set = 0, binding = 22, std430) restrict readonly buffer ClusterBuffer {
	uint data[];
}
cluster_buffer;

layout(set = 0, binding = 0, std140) uniform SceneDataBlock {
	SceneData data;
	SceneData prev_data;
}
scene_data_block;

bool sc_use_light_soft_shadows() {
	return false;
}

bool sc_use_light_projector() {
	return false;
}

bool sc_projector_use_mipmaps() {
	return false;
}

/* Reuse the Forward+ BRDF code directly instead of copying it, so upstream
 * changes are picked up automatically. The material BRDF defaults match the
 * ones the Forward+ shader compiler emits for StandardMaterial3D; the
 * optional material features compile out through their defines; the shadow
 * sampling functions compile out with SHADOWS_DISABLED. */
#define DIFFUSE_BURLEY
#define SPECULAR_SCHLICK_GGX
#define SHADOWS_DISABLED
#include "../scene_forward_lights_inc.glsl"

layout(location = 0) in vec2 uv_interp;

layout(push_constant, std430) uniform Params {
	uint directional_light_count;
	float luminance_multiplier;
	uint cluster_shift;
	uint cluster_width;
	uint cluster_type_size;
	uint max_cluster_element_count_div_32;
}
push_constant;

layout(set = 0, binding = 1, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
directional_lights;

/* G-buffer, filled by the geometry pass. */
layout(set = 0, binding = 2) uniform sampler2D gbuffer_albedo;
layout(set = 0, binding = 3) uniform sampler2D gbuffer_normal;
layout(set = 0, binding = 4) uniform sampler2D gbuffer_orm;
layout(set = 0, binding = 5) uniform sampler2D gbuffer_emission;
layout(set = 0, binding = 6) uniform sampler2D depth_buffer;

layout(set = 0, binding = 7) uniform sampler2D dfg;

// https://google.github.io/filament/Filament.html#toc5.3.4.7
// Note: We have an inverted Y-axis in the DFG; therefore, the roughness value must be inverted.
vec2 prefiltered_dfg(float lod, float NoV) {
	return textureLod(dfg, vec2(NoV, 1.0 - lod), 0.0).rg;
}

// Compute multiscatter compensation
// https://google.github.io/filament/Filament.html#listing_energycompensationimpl
vec3 get_energy_compensation(vec3 f0, float env) {
	return 1.0 + f0 * (1.0 / env - 1.0);
}

/* Cluster iteration helpers, same storage layout as the Forward+ scene
 * shader (scene_forward_clustered.glsl). */
void cluster_get_item_range(uint p_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
	uint item_min_max = cluster_buffer.data[p_offset];
	item_min = item_min_max & 0xFFFFu;
	item_max = item_min_max >> 16;

	item_from = item_min >> 5;
	item_to = (item_max == 0) ? 0 : ((item_max - 1) >> 5) + 1; //side effect of how it is stored, as item_max 0 means no elements
}

uint cluster_get_range_clip_mask(uint i, uint z_min, uint z_max) {
	int local_min = clamp(int(z_min) - int(i) * 32, 0, 31);
	int mask_width = min(int(z_max) - int(z_min), 32 - local_min);
	return bitfieldInsert(uint(0), uint(0xFFFFFFFF), local_min, mask_width);
}

layout(location = 0) out vec4 frag_color;

void main() {
	/* The G-buffer is filled through the inherited Forward+ material pass,
	 * whose projection is not flip_y corrected (unlike the main pass), so the
	 * G-buffer is vertically flipped: sample with the Y axis inverted. */
	vec2 gb_uv = vec2(uv_interp.x, 1.0 - uv_interp.y);

	float depth = texture(depth_buffer, gb_uv).r;
	if (depth == 0.0) {
		// Background: leave the clear color (or sky) behind.
		discard;
	}

	vec4 albedo_alpha = texture(gbuffer_albedo, gb_uv);
	vec4 normal_encoded = texture(gbuffer_normal, gb_uv);
	vec4 orm = texture(gbuffer_orm, gb_uv);
	vec4 emission = texture(gbuffer_emission, gb_uv);

	vec3 albedo = albedo_alpha.rgb;
	// The G-buffer normal and the directional light data are in view space,
	// like everything in the Forward+ shaders.
	vec3 normal = normalize(normal_encoded.rgb * 2.0 - 1.0);
	float ao = orm.r;
	float roughness = orm.g;
	float metallic = orm.b;

	// Reconstruct the view-space position from the depth buffer.
	vec3 ndc = vec3(uv_interp * 2.0 - 1.0, depth);
	vec4 view_position_h = scene_data_block.data.inv_projection_matrix * vec4(ndc, 1.0);
	vec3 view_position = view_position_h.xyz / view_position_h.w;
	vec3 view_dir = normalize(-view_position);

	// Ambient contribution.
	vec4 ambient_light = scene_data_block.data.ambient_light_color_energy;
	vec3 ambient = ambient_light.rgb * ambient_light.a;

	vec3 diffuse = vec3(0.0);
	vec3 specular = vec3(0.0);

	hvec3 f0 = F0(half(metallic), half(0.5), hvec3(albedo));

	// Multiscatter energy compensation, like the Forward+ opaque pass.
	float NdotV = clamp(dot(normal, view_dir), 0.0001, 1.0);
	vec2 envBRDF = prefiltered_dfg(roughness, NdotV);
	hvec3 energy_compensation = hvec3(get_energy_compensation(vec3(f0), envBRDF.y));

	half alpha = half(1.0);
	for (uint i = 0u; i < push_constant.directional_light_count; i++) {
		DirectionalLightData light = directional_lights.data[i];
		light_compute(hvec3(normal), normalize(hvec3(light.direction)), hvec3(view_dir), half(0.0),
				hvec3(light.color * light.energy),
				true,
				half(1.0) /* no shadows yet */,
				f0, half(roughness), half(metallic), half(light.specular),
				hvec3(albedo),
				alpha,
				uv_interp,
				energy_compensation,
				diffuse, specular);
	}

	/* Clustered positional lights (step B2): iterate the cluster baked by
	 * the inherited Forward+ cluster builder, same buffer layout as the
	 * Forward+ scene shader but without the subgroup optimizations (the
	 * shading pass is a fullscreen pass, forward divergence is negligible).
	 * Light cull masks and lightmap baked-light culling are not applied:
	 * the G-buffer carries no per-pixel instance data. */
	{
		// The shading pass renders the internal texture 1:1, so its
		// gl_FragCoord matches the cluster grid orientation.
		uvec2 cluster_pos = uvec2(gl_FragCoord.xy) >> push_constant.cluster_shift;
		uint cluster_offset = (push_constant.cluster_width * cluster_pos.y + cluster_pos.x) * (push_constant.max_cluster_element_count_div_32 + 32);
		uint cluster_z = uint(clamp((-view_position.z / scene_data_block.data.z_far) * 32.0, 0.0, 31.0));

		// Only used by soft shadows and light projectors, both stubbed off.
		vec3 vertex_dd = vec3(0.0);

		{ //omni lights
			uint cluster_omni_offset = cluster_offset;

			uint item_min;
			uint item_max;
			uint item_from;
			uint item_to;

			cluster_get_item_range(cluster_omni_offset + push_constant.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

			for (uint i = item_from; i < item_to; i++) {
				uint mask = cluster_buffer.data[cluster_omni_offset + i];
				mask &= cluster_get_range_clip_mask(i, item_min, item_max);

				while (mask != 0) {
					uint bit = findMSB(mask);
					mask &= ~(1u << bit);

					uint light_index = 32u * i + bit;
					light_process_omni(light_index, view_position, hvec3(view_dir), hvec3(normal), vertex_dd, vertex_dd, f0, half(roughness), half(metallic), scene_data_block.data.taa_frame_count, hvec3(albedo), alpha, uv_interp, energy_compensation,
							diffuse, specular);
				}
			}
		}

		{ //spot lights
			uint cluster_spot_offset = cluster_offset + push_constant.cluster_type_size;

			uint item_min;
			uint item_max;
			uint item_from;
			uint item_to;

			cluster_get_item_range(cluster_spot_offset + push_constant.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

			for (uint i = item_from; i < item_to; i++) {
				uint mask = cluster_buffer.data[cluster_spot_offset + i];
				mask &= cluster_get_range_clip_mask(i, item_min, item_max);

				while (mask != 0) {
					uint bit = findMSB(mask);
					mask &= ~(1u << bit);

					uint light_index = 32u * i + bit;
					light_process_spot(light_index, view_position, hvec3(view_dir), hvec3(normal), vertex_dd, vertex_dd, f0, half(roughness), half(metallic), scene_data_block.data.taa_frame_count, hvec3(albedo), alpha, uv_interp, energy_compensation,
							diffuse, specular);
				}
			}
		}
	}

	// Composition, mirroring the Forward+ opaque pass: diffuse and ambient
	// are modulated by albedo, ao and (1 - metallic); specular by ao.
	vec3 color = albedo * (ambient + diffuse) * ao * (1.0 - metallic) + specular * ao + emission.rgb;
	frag_color = vec4(color * push_constant.luminance_multiplier, 1.0);
}
