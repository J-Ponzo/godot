/* SHEngine fork addition: SHE-deferred fullscreen debug view (chantier sdk,
 * batch 2A3).
 *
 * Passthrough blit, written by the batch 2A3 debug/dump machinery:
 * - the Viewport.debug_draw SHE values display one G-buffer attachment as
 *   the visible frame, through the inherited post-process chain (the
 *   deferred flavor of the debug draw of the other renderers);
 * - the gbuffer dump (RenderingServer::she_request_gbuffer_dump()) copies
 *   the linear HDR color buffer into the readback-owned dump frame texture
 *   before tonemapping.
 *
 * The sampled values are exactly what the source holds: no decode, no
 * rescale (the dump is the byte-faithful oracle of the G-buffer oracle).
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

layout(location = 0) in vec2 uv_interp;

layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D source;

void main() {
	frag_color = texture(source, uv_interp);
}
