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
- Sky is deliberately NOT supported yet (no harness scene needs it, and it
  complicates `_render_scene`); it will come back as an intentional feature
  once a sky scene enters the suite.
- Deferred-specific limitations are tracked by the render harness
  (`harness/config.json` ignored scenes), never silently degraded.

## Roadmap

- **Step A (done)**: plumbing. The renderer is selectable (project settings,
  project manager, CLI).
- **Step B0 (done)**: G-buffer allocation. `RenderBufferDataSHEDeferred`
  allocates the deferred textures for every configured render buffers.
- **Step B1 (done)**: deferred core passes.
  - Geometry pass: opaque meshes rendered into the G-buffer by reusing the
    Forward+ material-mode pipelines (`_render_material`, MODE_RENDER_MATERIAL)
    through inheritance. No surface shader duplication: the upstream material
    shader already writes albedo/normal/ORM/emission/view-space depth.
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
- **Step B3+**: shadow maps, transparent-object pass (intentional Forward+
  reuse case), sky, GI/effects, as prioritized by the harness scenes.

## G-buffer layout

Owned by `RenderBufferDataSHEDeferred`, allocated under the
`RB_SCOPE_SHE_DEFERRED` context of `RenderSceneBuffersRD`. The geometry pass
goes through the Forward+ material-mode pipelines, so the layout mirrors the
bake framebuffer of `RendererSceneRenderRD::bake_render_uv2` exactly:

| Texture                   | Format   | Contents                                        |
|---------------------------|----------|-------------------------------------------------|
| `gbuffer_albedo`          | RGBA16F  | albedo.rgb, alpha                               |
| `gbuffer_normal`          | RGBA8    | 24-bit encoded view-space normal in rgb, a unused |
| `gbuffer_orm`             | RGBA8    | ao (r), roughness (g), metallic (b), sss (a)    |
| `gbuffer_emission`        | RGBA16F  | emission.rgb                                    |
| `gbuffer_depth`           | R32F     | view-space linear depth (-vertex.z)             |
| `gbuffer_depth_attachment`| D32/X8D24| deferred depth (z-test for the geometry pass)    |

Encoding notes:

- The G-buffer is filled through the inherited `_render_material()` whose
  projection is not flip_y-corrected (unlike the main pass), so the whole
  G-buffer is vertically flipped: the shading pass samples with the Y axis
  inverted. The shared base depth buffer is deliberately NOT written by the
  geometry pass.
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
| `forward_clustered/render_forward_clustered.h` | one `private:` → `protected:` section | lets `RenderSHEDeferred` reuse the Forward+ machinery instead of duplicating it |
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
- `_render_material()` (Forward+ protected virtual) is the geometry pass. If
  upstream changes its signature or its outputs, the G-buffer layout section
  above must be updated to match.
- `LightStorage::update_light_buffers()` must be called by the deferred path
  itself (Forward+ does it in `_pre_opaque_render`, which the deferred path
  does not run).
- The DFG LUT is regenerated at startup by our renderer, and the area light
  LTC LUTs are created lazily at first render (RS is not ready during
  construction), both through shared data/classes (the Forward+ members are
  private).

## Known limitations (B2)

- Directional + clustered positional (omni/spot) lights + ambient; positional
  light cull masks and lightmap baked-light culling are not applied (no
  per-pixel instance data in the G-buffer).
- No shadow maps, no GI, no volumetrics, no transparent objects, no MSAA
  (warned once and ignored), no reflection probes / multiview (error + skip),
  no sky (error + clear color).
