/* SHEngine fork addition: SHE-deferred fullscreen lighting pass (step B1).
 *
 * Reads the G-buffer filled by the deferred geometry pass (which reuses the
 * Forward+ material-mode pipelines, see render_she_deferred.cpp) and applies
 * ambient, directional light and clustered positional light (omni/spot, step
 * B2) contributions, with shadow maps (directional PSSM, omni dual
 * paraboloid/cube, spot; step B3), into the HDR color buffer. GI and
 * volumetrics are not implemented yet.
 *
 * Shadow map sampling reuses the shared Forward+ light processing functions
 * (scene_forward_lights_inc.glsl) for the positional lights and the shared
 * directional PSSM function (scene_forward_directional_shadow_inc.glsl,
 * extracted from the Forward+ scene shader so both methods run the exact
 * same math; see the README sync checklist).
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
#include "../decal_data_inc.glsl"

#define M_PI 3.14159265359
#define MAX_ROUGHNESS_LOD 5 /* same value as the Forward+ scene shader */
#define REFLECTION_MULTIPLIER 1.0 /* same value as the Forward+ scene shader */

#include "../oct_inc.glsl"

/* Declarations required by the shared Forward+ light processing functions
 * (positional light buffers and decal atlas are shared storages; the
 * projector helpers are stubbed until light projectors land). */
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

/* Decal data (step B5), filled by TextureStorage::update_decal_buffer()
 * exactly as in the Forward+ _pre_opaque_render(); the decal atlas binding
 * below samples albedo/emission through the srgb view and normal/ORM through
 * the linear one, like the Forward+ decal block. */
layout(set = 0, binding = 26, std430) restrict readonly buffer Decals {
	DecalData data[];
}
decals;

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

/* Decal atlas sampler, selected per decals/filter project setting like the
 * Forward+ render pass set (the lighting pass binds it through its own set,
 * the Forward+ material pipelines through theirs). */
layout(set = 0, binding = 27) uniform sampler decal_sampler;

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

/* Shadow maps (step B3): same textures and comparison sampler as the
 * Forward+ scene pass (a single nearest/COMPARE_GREATER sampler is shared by
 * the directional and positional atlases). The renderer binds the default
 * depth texture when the atlas does not exist, like Forward+. */
layout(set = 0, binding = 23) uniform texture2D directional_shadow_atlas;
layout(set = 0, binding = 24) uniform texture2D shadow_atlas;
layout(set = 0, binding = 25) uniform sampler shadow_sampler;

layout(push_constant, std430) uniform Params {
	uint directional_light_count;
	float luminance_multiplier;
	uint cluster_shift;
	uint cluster_width;
	uint cluster_type_size;
	uint max_cluster_element_count_div_32;
	uint feature_flags; /* bit 0: directional light soft shadows, bit 1: positional light soft shadows, bit 2: multi-bounce ambient occlusion, bit 3: decal atlas mipmapped filtering */
	uint soft_shadow_samples;
	uint penumbra_shadow_samples;
	uint directional_soft_shadow_samples;
	uint directional_penumbra_shadow_samples;
}
push_constant;

bool sc_use_light_soft_shadows() {
	return (push_constant.feature_flags & 2u) != 0u;
}

bool sc_use_directional_soft_shadows() {
	return (push_constant.feature_flags & 1u) != 0u;
}

bool sc_use_light_projector() {
	return false;
}

bool sc_projector_use_mipmaps() {
	return false;
}

/* Decal atlas mipmapped filtering mirrors the Forward+ pipeline
 * specialization (decal_use_mipmaps, see SceneShaderForwardClustered): the
 * quality setting is read at frame time and passed through the push constant
 * (feature_flags bit 3), like the shadow filter sample counts below. */
bool sc_decal_use_mipmaps() {
	return (push_constant.feature_flags & 8u) != 0u;
}

/* Shadow filter sample counts: filled from the shared quality getters at
 * frame time (they are only valid after RendererSceneRenderRD::init()
 * applies the project settings, and can change at runtime); the Forward+
 * pipeline gets them through its specialization constants. */
uint sc_soft_shadow_samples() {
	return push_constant.soft_shadow_samples;
}

uint sc_penumbra_shadow_samples() {
	return push_constant.penumbra_shadow_samples;
}

uint sc_directional_soft_shadow_samples() {
	return push_constant.directional_soft_shadow_samples;
}

uint sc_directional_penumbra_shadow_samples() {
	return push_constant.directional_penumbra_shadow_samples;
}

layout(set = 0, binding = 1, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
directional_lights;

/* Reuse the Forward+ BRDF code directly instead of copying it, so upstream
 * changes are picked up automatically. The material BRDF defaults match the
 * ones the Forward+ shader compiler emits for StandardMaterial3D; the
 * optional material features compile out through their defines; the
 * positional-light shadow sampling functions come from the same include
 * (the directional PSSM one from the shared include below it). */
#define DIFFUSE_BURLEY
#define SPECULAR_SCHLICK_GGX
#include "../scene_forward_lights_inc.glsl"

#include "../scene_forward_directional_shadow_inc.glsl"

layout(location = 0) in vec2 uv_interp;

/* G-buffer, filled by the geometry pass. */
layout(set = 0, binding = 2) uniform sampler2D gbuffer_albedo;
layout(set = 0, binding = 3) uniform sampler2D gbuffer_normal;
layout(set = 0, binding = 4) uniform sampler2D gbuffer_orm;
layout(set = 0, binding = 5) uniform sampler2D gbuffer_emission;
layout(set = 0, binding = 6) uniform sampler2D depth_buffer;

layout(set = 0, binding = 7) uniform sampler2D dfg;

// https://google.github.io/Filament/Filament.html#toc5.3.4.7
// Note: We have an inverted Y-axis in the DFG; therefore, the roughness value must be inverted.
vec2 prefiltered_dfg(float lod, float NoV) {
	return textureLod(dfg, vec2(NoV, 1.0 - lod), 0.0).rg;
}

// Compute multiscatter compensation
// https://google.github.io/Filament/Filament.html#listing_energycompensationimpl
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
	/* The G-buffer is filled through the Forward+ material-mode pipelines
	 * driven with the main camera data (see render_she_deferred.cpp), so it
	 * is in main-pass orientation: sample the render targets directly. */
	float depth = texture(depth_buffer, uv_interp).r;
	if (depth == 0.0) {
		// Background: leave the clear color (or sky) behind.
		discard;
	}

	vec4 albedo_alpha = texture(gbuffer_albedo, uv_interp);
	vec4 normal_encoded = texture(gbuffer_normal, uv_interp);
	vec4 orm = texture(gbuffer_orm, uv_interp);
	vec4 emission = texture(gbuffer_emission, uv_interp);

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

	// Screen-space derivatives of the fragment position, like the Forward+
	// scene pass (vertex_ddx/vertex_ddy there): the soft positional shadow
	// penumbra shapes are built from them, and the decal block below uses
	// them as the atlas sampling gradients when mipmapped decal filtering is
	// enabled.
	vec3 vertex_ddx = dFdx(view_position);
	vec3 vertex_ddy = dFdy(view_position);

	/* Cluster iteration state shared by the decal and light sections below:
	 * same storage layout as the Forward+ scene shader
	 * (scene_forward_clustered.glsl). The shading pass renders the internal
	 * texture 1:1, so its gl_FragCoord matches the cluster grid orientation. */
	uvec2 cluster_pos = uvec2(gl_FragCoord.xy) >> push_constant.cluster_shift;
	uint cluster_offset = (push_constant.cluster_width * cluster_pos.y + cluster_pos.x) * (push_constant.max_cluster_element_count_div_32 + 32);
	uint cluster_z = uint(clamp((-view_position.z / scene_data_block.data.z_far) * 32.0, 0.0, 31.0));

	{
		/* Decals (step B5): iterate the cluster baked by the inherited
		 * Forward+ cluster builder (decals live in cluster type slot 3, the
		 * same offset as in the Forward+ decal block), and apply them to the
		 * G-buffer samples before any lighting math, mirroring the upstream
		 * decal block of the opaque pass word for word. Differences are
		 * structural, documented in the she_deferred README:
		 * - the decal transforms act on the view-space position, which the
		 *   Forward+ scene pass interpolates per fragment and this pass
		 *   reconstructs from the depth buffer (mathematically the same
		 *   value on a surface, modulo fp and the depth encode);
		 * - decal cull masks vs visual instance layers are not applied: the
		 *   G-buffer carries no per-pixel instance data (same limitation
		 *   class as the light cull masks, see the README);
		 * - normal_fade is evaluated against the base G-buffer shading
		 *   normal where Forward+ uses the geometric normal (not stored in
		 *   the G-buffer): identical on plain materials, the only decal-block
		 *   caller of it;
		 * - like the light sections below, the Forward+ subgroup
		 *   optimizations compile out: the per-fragment decal application
		 *   order (for i ascending, bits within i descending) is identical,
		 *   only the execution grouping differs. */
		uint cluster_decal_offset = cluster_offset + push_constant.cluster_type_size * 3u;

		uint item_min;
		uint item_max;
		uint item_from;
		uint item_to;

		cluster_get_item_range(cluster_decal_offset + push_constant.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);

		// Forward+ evaluates normal_fade against the (constant) geometric
		// normal; the base decoded G-buffer normal stands in for it here, so
		// earlier decals in the loop cannot influence later normal_fades.
		vec3 base_normal = normal;

		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[cluster_decal_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);

			while (mask != 0) {
				uint bit = findMSB(mask);
				mask &= ~(1u << bit);

				uint decal_index = 32u * i + bit;

				vec3 uv_local = (decals.data[decal_index].xform * vec4(view_position, 1.0)).xyz;
				if (any(lessThan(uv_local, vec3(0.0, -1.0, 0.0))) || any(greaterThan(uv_local, vec3(1.0)))) {
					continue; //out of decal
				}

				float fade = pow(1.0 - (uv_local.y > 0.0 ? uv_local.y : -uv_local.y), uv_local.y > 0.0 ? decals.data[decal_index].upper_fade : decals.data[decal_index].lower_fade);

				if (decals.data[decal_index].normal_fade > 0.0) {
					fade *= smoothstep(decals.data[decal_index].normal_fade, 1.0, dot(base_normal, decals.data[decal_index].normal) * 0.5 + 0.5);
				}

				//we need ddx/ddy for mipmaps, so simulate them
				vec2 ddx = (decals.data[decal_index].xform * vec4(vertex_ddx, 0.0)).xz;
				vec2 ddy = (decals.data[decal_index].xform * vec4(vertex_ddy, 0.0)).xz;

				if (decals.data[decal_index].albedo_rect != vec4(0.0)) {
					//has albedo
					vec4 decal_albedo;
					if (sc_decal_use_mipmaps()) {
						decal_albedo = textureGrad(sampler2D(decal_atlas_srgb, decal_sampler), uv_local.xz * decals.data[decal_index].albedo_rect.zw + decals.data[decal_index].albedo_rect.xy, ddx * decals.data[decal_index].albedo_rect.zw, ddy * decals.data[decal_index].albedo_rect.zw);
					} else {
						decal_albedo = textureLod(sampler2D(decal_atlas_srgb, decal_sampler), uv_local.xz * decals.data[decal_index].albedo_rect.zw + decals.data[decal_index].albedo_rect.xy, 0.0);
					}
					decal_albedo *= decals.data[decal_index].modulate;
					decal_albedo.a *= fade;
					albedo = mix(albedo, decal_albedo.rgb, decal_albedo.a * decals.data[decal_index].albedo_mix);

					if (decals.data[decal_index].normal_rect != vec4(0.0)) {
						vec3 decal_normal;
						if (sc_decal_use_mipmaps()) {
							decal_normal = textureGrad(sampler2D(decal_atlas, decal_sampler), uv_local.xz * decals.data[decal_index].normal_rect.zw + decals.data[decal_index].normal_rect.xy, ddx * decals.data[decal_index].normal_rect.zw, ddy * decals.data[decal_index].normal_rect.zw).xyz;
						} else {
							decal_normal = textureLod(sampler2D(decal_atlas, decal_sampler), uv_local.xz * decals.data[decal_index].normal_rect.zw + decals.data[decal_index].normal_rect.xy, 0.0).xyz;
						}
						decal_normal.xy = decal_normal.xy * vec2(2.0, -2.0) - vec2(1.0, -1.0); //users prefer flipped y normal maps in most authoring software
						decal_normal.z = sqrt(max(0.0, 1.0 - dot(decal_normal.xy, decal_normal.xy)));
						//convert to view space, use xzy because y is up
						decal_normal = (decals.data[decal_index].normal_xform * decal_normal.xzy).xyz;

						normal = normalize(mix(normal, decal_normal, decal_albedo.a));
					}

					if (decals.data[decal_index].orm_rect != vec4(0.0)) {
						vec3 decal_orm;
						if (sc_decal_use_mipmaps()) {
							decal_orm = textureGrad(sampler2D(decal_atlas, decal_sampler), uv_local.xz * decals.data[decal_index].orm_rect.zw + decals.data[decal_index].orm_rect.xy, ddx * decals.data[decal_index].orm_rect.zw, ddy * decals.data[decal_index].orm_rect.zw).xyz;
						} else {
							decal_orm = textureLod(sampler2D(decal_atlas, decal_sampler), uv_local.xz * decals.data[decal_index].orm_rect.zw + decals.data[decal_index].orm_rect.xy, 0.0).xyz;
						}
						ao = mix(ao, decal_orm.r, decal_albedo.a);
						roughness = mix(roughness, decal_orm.g, decal_albedo.a);
						metallic = mix(metallic, decal_orm.b, decal_albedo.a);
					}
				}

				if (decals.data[decal_index].emission_rect != vec4(0.0)) {
					//emission is additive, so its independent from albedo
					if (sc_decal_use_mipmaps()) {
						emission.rgb += textureGrad(sampler2D(decal_atlas_srgb, decal_sampler), uv_local.xz * decals.data[decal_index].emission_rect.zw + decals.data[decal_index].emission_rect.xy, ddx * decals.data[decal_index].emission_rect.zw, ddy * decals.data[decal_index].emission_rect.zw).xyz * decals.data[decal_index].modulate.rgb * decals.data[decal_index].emission_energy * fade;
					} else {
						emission.rgb += textureLod(sampler2D(decal_atlas_srgb, decal_sampler), uv_local.xz * decals.data[decal_index].emission_rect.zw + decals.data[decal_index].emission_rect.xy, 0.0).xyz * decals.data[decal_index].modulate.rgb * decals.data[decal_index].emission_energy * fade;
					}
				}
			}
		}
	}

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
		float shadow = 1.0;
		if (light.shadow_opacity > 0.001) {
			/* The shared Forward+ PSSM sampling (see the include above);
			 * the G-buffer shading normal stands in for Forward+'s
			 * geometric normal (no geometric normal in the G-buffer,
			 * documented in the she_deferred README). */
			shadow = forward_sample_directional_shadow(i, view_position, normal, scene_data_block.data);
			// Distance fade (fade_from/fade_to are negative like view_position.z).
			shadow = mix(shadow, 1.0, smoothstep(light.fade_from, light.fade_to, view_position.z));
			// Forward+ quantizes the shadow to 8 bits through shadow0/shadow1
			// before the lighting loop consumes it; do the same here.
			shadow = float(uint(clamp(shadow * 255.0, 0.0, 255.0))) / 255.0;
			shadow = mix(1.0, shadow, light.shadow_opacity);
		}
		float size_A = sc_use_directional_soft_shadows() ? light.size : 0.0;
		light_compute(hvec3(normal), normalize(hvec3(light.direction)), hvec3(view_dir), half(size_A),
				hvec3(light.color * light.energy),
				true,
				half(shadow),
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
	 * The cluster iteration state (cluster_pos/cluster_offset/cluster_z)
	 * is computed above, before the decal section. Light cull masks and
	 * lightmap baked-light culling are not applied: the G-buffer carries no
	 * per-pixel instance data. */
	{
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
					light_process_omni(light_index, view_position, hvec3(view_dir), hvec3(normal), vertex_ddx, vertex_ddy, f0, half(roughness), half(metallic), scene_data_block.data.taa_frame_count, hvec3(albedo), alpha, uv_interp, energy_compensation,
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
					light_process_spot(light_index, view_position, hvec3(view_dir), hvec3(normal), vertex_ddx, vertex_ddy, f0, half(roughness), half(metallic), scene_data_block.data.taa_frame_count, hvec3(albedo), alpha, uv_interp, energy_compensation,
							diffuse, specular);
				}
			}
		}
	}

	/* Composition, mirroring the Forward+ opaque pass
	 * (scene_forward_clustered.glsl): ambient occlusion only modulates the
	 * ambient term (with the multi-bounce approximation when the project
	 * setting enables it); the direct diffuse/specular terms use
	 * mix(1.0, ao, ao_light_affect) instead, and that material parameter
	 * (default 0) is not stored in the G-buffer: the deferred path applies no
	 * AO on direct lighting (documented in the she_deferred README). */
	vec3 ambient_ao;
	if ((push_constant.feature_flags & 4u) != 0u) {
		// Multi-bounce ambient-occlusion approximation to ambient light:
		// https://blog.selfshadow.com/publications/s2016-shading-course/activision/s2016_pbs_activision_occlusion.pdf#page=78
		vec3 a = vec3(2.0404) * albedo - vec3(0.3324);
		vec3 b = vec3(-4.7951) * albedo + vec3(0.6417);
		vec3 c = vec3(2.7552) * albedo + vec3(0.6903);
		vec3 x = vec3(ao);
		ambient_ao = max(x, ((x * a + b) * x + c) * x);
	} else {
		ambient_ao = vec3(ao);
	}

	vec3 color = albedo * (ambient * ambient_ao + diffuse) * (1.0 - metallic) + specular + emission.rgb;
	frag_color = vec4(color * push_constant.luminance_multiplier, 1.0);
}
