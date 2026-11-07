# SHE-deferred renderer (SHEngine fork addition)

Additive deferred renderer living next to Forward+ and Mobile
(`servers/rendering/renderer_rd/`). Selected with the `she_deferred`
rendering method. The goal is **minimal divergence from upstream Godot**:
everything lives in new files, upstream files are only touched where
unavoidable, and every such touch is listed below.

## Design principles

- `RenderSHEDeferred` inherits from `RenderForwardClustered` to reuse as much
  as possible of the RD scene infrastructure: storages, light buffers,
  render lists, environment setup, post-process. Fork-specific code lives
  here and in `shaders/she_deferred/`, nowhere else.
- Shader-side reuse is done by **`#include`-ing the Forward+ shader building
  blocks** (`scene_forward_lights_inc.glsl` and the shared `*_inc.glsl`
  data includes) rather than copying them, so upstream changes are picked up
  automatically. Forward-only code paths inside those includes compile out
  through their defines (`SHADOWS_DISABLED`, absent feature defines).
- **No implicit fallback to Forward+.** Features the deferred path does not
  support fail loudly (error message + skipped render / ignored setting), so
  a missing feature is always visible. Falling back to Forward+ is done
  intentionally and case by case (e.g. a dedicated transparent-object pass
  later), never silently. The step A "magenta render target" was the extreme
  form of this principle.
- Sky is still NOT supported (step B3 scope): the harness `smoke_001` scene
  needs a procedural sky and stays ignored in `harness/config.json` for that
  reason; it will come back as an intentional feature once sky lands.
- Deferred-specific limitations are tracked by the render harness
  (`harness/config.json` ignored scenes), never silently degraded.

## Roadmap

- **Step A (done)**: plumbing. The renderer is selectable (project settings,
  project manager, CLI).
- **Step B0 (done)**: G-buffer allocation. `RenderBufferDataSHEDeferred`
  allocates the deferred textures for every configured render buffers.
- **Step B1 (done)**: deferred core passes.
  - Geometry pass: opaque meshes rendered into the G-buffer by reusing the
    Forward+ material-mode pipelines (MODE_RENDER_MATERIAL). No surface shader
    duplication: the upstream material shader already writes
    albedo/normal/ORM/emission/view-space depth. The pass is orchestrated by
    `RenderSHEDeferred::_render_scene()` itself (the same inherited pieces as
    the upstream `_render_material()` helper: `_setup_environment()`,
    `_fill_render_list(RENDER_LIST_SECONDARY, ..., PASS_MODE_DEPTH_MATERIAL)`,
    `_render_list_with_draw_list()`), but with the real render data instead of
    the helper's own local scene data: the geometry is rasterized with the main
    camera projection (flip_y corrected), so the G-buffer and the shared base
    depth buffer written alongside it are in main-pass orientation, with the
    same depth conventions as every Forward+ pass (reversed-Z,
    `COMPARE_OP_GREATER_OR_EQUAL`, clear 0.0). This is what will let a reused
    Forward+ transparent pass depth-test against that buffer as-is, with no
    depth copy/flip pass and no extra depth texture.
  - Shading pass (`she_deferred_lighting.glsl`, the only new GLSL): fullscreen
    pass reading the G-buffer, applying ambient + directional light
    contributions (Forward+ `light_compute`, incl. Burley diffuse, Schlick-GGX
    specular, multiscatter energy compensation) into the HDR color buffer,
    before the inherited post-process chain. Step B2 added the clustered
    positional light (omni/spot) iteration on top (see step B2 below).
  - Cross-comparison parity vs the Forward+ oracle on the B1 scope scenes
    (`brdf_chart_001`, `lights_many_directional_001`): rmse < 0.7 (filmic
    tonemapped, 8-bit), determinism bit-perfect.
- **Step B2 (done)**: clustered positional lights (omni/spot) in the shading pass.
  - The inherited Forward+ cluster builder (living on the Forward+ render
    buffer data, `RB_SCOPE_FORWARD_CLUSTERED`) is driven directly by
    `_render_scene()`: `begin()`, then `LightStorage::update_light_buffers()`
    (whose `setup_added_light()` hook feeds the builder), then `bake_cluster()`.
  - The shading pass iterates the cluster buffer with the same storage layout
    and helpers as the Forward+ scene shader (minus its subgroup
    optimizations), calling the shared `light_process_omni`/`light_process_spot`
    from `scene_forward_lights_inc.glsl`. Cluster layout parameters
    (`cluster_shift/width/type_size`, element count div 32) are passed through
    the lighting pass push constant, computed with the same math as
    `RenderForwardClustered::_setup_environment()`.
  - Not applied (no per-pixel instance data in the G-buffer): light cull masks
    vs visual instance layers, and skipping statically-baked lights on
    lightmapped instances. No harness scene in the current scope uses either.
- **Step B3 (done)**: shadow maps (directional PSSM, omni cube/dual
  paraboloid, spot).
  - The shadow passes are rendered through the inherited `_render_shadow_passes()`,
    the shadow subset Forward+ extracted from `_pre_opaque_render()`
    (classification, cube shadows, directional atlas, directional/positional
    passes; passed at the same call site so Forward+'s behavior is
    bit-identical). They run before the deferred path refreshes the light
    buffers, like Forward+ whose shadow passes run between its two
    `_setup_environment()` calls, so the shadow state they mutate
    (directional atlas rects, finalized omni cubemaps) is picked up by the
    buffer update that feeds the shading pass. The comparison sampler is the
    Forward+ public `shadow_sampler`.
  - Shading side, the positional omni/spot shadows are sampled by the shared
    `light_process_omni`/`light_process_spot` from
    `scene_forward_lights_inc.glsl` after un-defining `SHADOWS_DISABLED`;
    the directional PSSM sampling is the shared `forward_sample_directional_shadow()`
    from `scene_forward_directional_shadow_inc.glsl`, extracted verbatim from
    the Forward+ scene shader for this purpose (no copy). The deferred caller
    keeps the distance fade and the 8-bit shadow quantization Forward+
    applies through shadow0/shadow1. SSCS, lightmap shadowmask modes,
    per-pixel light cull masks and vertex lighting are not part of the
    deferred path.
  - Specialization helpers come through the push constant at frame time:
    the two soft-shadow flags mirror the Forward+ pipeline
    specializations (directional: from `LightStorage::update_light_buffers()`;
    positional: scene-global, Forward+ derives it per material instance from
    its light lists, `uses_softshadow` == light size > 0 or area light, and
    the per-light size checks inside the shared functions keep the result
    identical pixel by pixel), and the four sample counts are read from the
    shared quality getters. Frame time matters: the getters only return the
    project settings after `RendererSceneRenderRD::init()` runs, after the
    renderer constructors (an earlier constructor snapshot made the
    deferred shadows single-sample sharp vs Forward+'s disk PCF - the
    `shadows_001` cross failure).
  - AO composition (surfaced by `texture_zoo_001`): Forward+ applies the
    material ambient occlusion to the ambient term only, through
    `ao = mix(1.0, ao, ao_light_affect)` for the direct diffuse/specular
    terms (`scene_forward_clustered.glsl`, direct-light AO); the deferred
    composition mirrors the default (ambient * ao, incl. the multi-bounce
    approximation behind the `multi_bounce_occlusion` project setting), and
    AO Light Affect itself is not recoverable from the G-buffer: the
    deferred path applies no AO on direct light for materials that set it.
  - Known parity limitation: Forward+ biases the directional sampling
    position with the geometric (pre-normal-map) normal; the G-buffer only
    stores the final shading normal, which is used in its place. Identical
    for non-normal-mapped surfaces, modulo the RGBA8 normal encoding.
  - Height maps (deep parallax) come with the material-mode pipelines and
    now match the Forward+ main pass: the residue that used to keep
    `texture_zoo_001` harness-ignored was the old geometry pass's
    un-flip_y-corrected projection, gone since the pass renders with the
    main camera data (cross rmse=0.1324, max_delta=5, strict defaults,
    evidence in harness/KNOWN_ISSUES.md).
- **Step B3+**: transparent-object pass (intentional Forward+ reuse case),
  sky, GI/effects, as prioritized by the harness scenes.

## G-buffer layout

Owned by `RenderBufferDataSHEDeferred`, allocated under the
`RB_SCOPE_SHE_DEFERRED` context of `RenderSceneBuffersRD`. The geometry pass
goes through the Forward+ material-mode pipelines, so the color layout
mirrors the bake framebuffer of `RendererSceneRenderRD::bake_render_uv2`:

| Texture             | Format   | Contents                                          |
|---------------------|----------|---------------------------------------------------|
| `gbuffer_albedo`    | RGBA16F  | albedo.rgb, alpha                                 |
| `gbuffer_normal`    | RGBA8    | 24-bit encoded view-space normal in rgb, a unused |
| `gbuffer_orm`       | RGBA8    | ao (r), roughness (g), metallic (b), sss (a)      |
| `gbuffer_emission`  | RGBA16F  | emission.rgb                                      |
| `gbuffer_depth`     | R32F     | view-space linear depth (-vertex.z); mandatory 5th sink attachment of the material pipelines, never read back |

The depth attachment of the geometry-pass framebuffer is the shared base
depth buffer (`rb->get_depth_texture()`, D32/X8D24), not a deferred-owned
texture: the pass rasterizes with the main camera projection, so that depth
is valid for every consumer of the main-pass depth (the shading pass samples
it for position reconstruction, a reused Forward+ transparent pass will
depth-test against it). Reversed-Z, like every pass of this engine.

Encoding notes:

- The G-buffer is filled through the Forward+ material-mode pipelines driven
  with the main camera data (flip_y corrected), so it is in main-pass
  orientation: the shading pass samples the render targets directly, with no
  Y mirroring to undo.
- Normals, directional light data and the lighting math are in view space,
  like everything in the Forward+ shaders. Data is linear end to end; the
  sRGB conversion only happens in the inherited post-process.
- RGBA8 precision for normal/ORM is the B1 starting point (`normal_map_001`
  and `brdf_chart_001` are the precision oracles); formats can be widened
  per-texture without changing the pass structure.
- `gbuffer_albedo` was widened to RGBA16F after the
  `lights_many_directional_001` cross investigation: mid-gray albedo values
  (0.5/0.55 sit exactly between two UNORM8 levels, 127.5 -> 128) shift lit
  output by ~2/255 on broad surfaces, which alone failed the default cross
  ssim threshold (0.9952 vs 0.999; rmse was passing with margin). Widening
  the albedo only passes all in-scope scenes with the strict defaults
  (ssim >= 0.9998). The normal and ORM textures were bisected too:
  widening the normal made the razor-thin grazing specular flash of
  `brdf_chart_001` *worse* (max 10 -> 13), so keep them at RGBA8 unless a
  future scene justifies revisiting.

## Upstream files touched (sync checklist)

Every modification to a non-`she_deferred` file, and why. After an upstream
merge, re-check each entry still applies and still compiles:

| File | Change | Why |
|------|--------|-----|
| `renderer_rd/SCsub` | +1 `SConscript("she_deferred/SCsub")` | build the renderer |
| `renderer_rd/renderer_compositor_rd.cpp` | include + `she_deferred` branch | renderer selection |
| `renderer_rd/shaders/SCsub` | +1 `SConscript("she_deferred/SCsub")` | build the shaders |
| `forward_clustered/render_forward_clustered.h` | one `private:` → `protected:` section; declaration of `_render_shadow_passes()`; the `RenderListType` enum exposed `protected` | lets `RenderSHEDeferred` reuse the Forward+ machinery (incl. driving the material-mode geometry pass itself) instead of duplicating it |
| `forward_clustered/render_forward_clustered.cpp` | shadow orchestration subset of `_pre_opaque_render()` extracted verbatim into `_render_shadow_passes()`, called at the same site | share the exact shadow passes with the deferred renderer; the Forward+ output must stay bit-identical (harness regression) |
| `shaders/forward_clustered/scene_forward_clustered.glsl` | directional PSSM block (fragment, 182 lines) extracted verbatim into the new shared `shaders/scene_forward_directional_shadow_inc.glsl`, called at the same site | share the exact directional shadow sampling with the deferred lighting pass; distance fade / lightmap / SSCS stay inline |
| `main/main.cpp` | method hints/whitelist/driver check | register the rendering method |
| `editor/project_manager/project_dialog.cpp`, `editor/editor_node.cpp`, `editor/settings/editor_settings.cpp`, `core/config/project_settings.cpp` | `she_deferred` entries | project manager / settings / supported features / default-renderer fallback |

Design notes for future merges:

- The Forward+ BRDF/light code is `#include`-d (not copied): if upstream
  renames or restructures `scene_forward_lights_inc.glsl`, our include breaks
  at build time (loud, fix by updating one include). The include requires a
  set of declarations in our shader (positional light buffers, decal/area
  light atlases, LTC and DFG LUTs, samplers, `sc_*` stubs, BRDF defines
  `DIFFUSE_BURLEY`/`SPECULAR_SCHLICK_GGX`, `SHADOWS_DISABLED`); if upstream
  adds new bindings referenced by that include, the build fails and the new
  declaration + uniform binding must be added (most are shared storages
  with public getters).
- The geometry pass reuses the Forward+ material-mode pipelines
  (`PASS_MODE_DEPTH_MATERIAL`) driven by `RenderSHEDeferred::_render_scene()`
  itself (not the upstream `_render_material()` helper, whose local scene data
  is not flip_y-corrected - using it also committed the G-buffer to a vertical
  mirror and a deferred-owned depth copy). If upstream changes the material
  shader's outputs, the G-buffer layout section above must be updated to
  match; if it changes `_fill_render_list`/`_render_list` plumbing or
  `_setup_environment()`, the deferred geometry pass follows it through the
  shared calls (loud at build time through the same signatures).
- The shadow orchestration (`_render_shadow_passes`, extracted from
  `_pre_opaque_render()`) and the directional PSSM sampling
  (`scene_forward_directional_shadow_inc.glsl`, extracted from
  `scene_forward_clustered.glsl`) are shared verbatim with Forward+; if
  upstream restructures either area, the extraction must be re-applied on
  the merge (both are loud: the deferred build or the bit-exact Forward+
  regression catches a drift).
- The lighting pass specialization helpers come through its push constant;
  the sample counts are read from the shared quality getters at frame time
  (valid only after `RendererSceneRenderRD::init()`), the soft-shadow flags
  per render. Do not snapshot them at construction.
- `LightStorage::update_light_buffers()` must be called by the deferred path
  itself (Forward+ does it in `_pre_opaque_render`, which the deferred path
  does not run).
- The DFG LUT is regenerated at startup by our renderer, and the area light
  LTC LUTs are created lazily at first render (RS is not ready during
  construction), both through shared data/classes (the Forward+ members are
  private).

## Known limitations (B3)

- Directional + clustered positional (omni/spot) lights + ambient, with shadow
  maps on all three light types; positional light cull masks and lightmap
  baked-light culling are not applied (no per-pixel instance data in the
  G-buffer). Contact shadows are not processed.
- Height maps (deep parallax) are supported through the material-mode
  pipelines: since the geometry pass renders with the main camera data,
  the parallax inputs match the Forward+ main pass and `texture_zoo_001`
  passes the strict cross defaults (rmse 0.1324, max_delta 5; evidence in
  harness/KNOWN_ISSUES.md).
- No GI, no volumetrics, no transparent objects, no MSAA (warned once and
  ignored), no reflection probes / multiview (error + skip), no sky (error +
  clear color).
- The directional shadow bias uses the shading normal where Forward+ uses the
  geometric normal (no geometric normal in the G-buffer).
