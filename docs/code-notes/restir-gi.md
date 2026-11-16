# ReSTIR GI (modules/gi_restir)

### `modules/gi_restir/config.py`

Reservoir resampling of the ray-traced diffuse pass (R2, see rtgi-r2.md): temporal and spatial reuse of each pixel's
traced sample, in front of R2's denoiser. `modules/gi_restir/config.py` builds the module with the engine (any build
with a RenderingDevice). It is off at run time until rendering/global_illumination/restir/gi/enabled is set; every
other restir setting defaults to false or neutral. The sections below follow the features in the order they were
added, each under the setting that switches it.

### `modules/gi_restir/register_types.cpp`

- SERVERS level: registers the RtgiResampleBackend (RestirGiBackend) with RendererRD::GIModuleHooks, and creates the
  probe singleton GiRestirProbe. The backend is always registered (run-time switching stays possible) and answers
  is_active() from rendering/global_illumination/restir/gi/enabled. The path tracer backend is not part of this line.
  Core treats a registered backend with is_active() false as no backend and releases what it held, so with the
  settings at their defaults the frame is the one rendered with no module. An active backend may still refuse a
  frame (`modules/gi_restir/restir_gi.cpp` lists the reasons in its status): core then runs the plain reconstruction
  and counts the refusal.
- SCENE level: registers the settings under rendering/global_illumination/restir/ (`modules/gi_restir/restir_settings.cpp`).
- Uninitialise (SERVERS): release_all, unregister_all, remove the probe singleton, free the two objects.

## Settings (`modules/gi_restir/restir_settings.cpp`)

| Name (under rendering/global_illumination/restir/) | Type | Default |
|---|---|---|
| gi/enabled | bool | false |
| gi/mode | enum B1,B2,B2k,U1 | B2 |
| gi/spatial_neighbors | int 0..8 | 3 |
| gi/spatial_radius | int 1..128 | 24 |
| gi/spatial_passes | int 1..2 | 1 |
| gi/m_temporal_cap | int 1..256 | 20 |
| gi/m_max | int 1..512 | 32 |
| gi/n_target | int 1..64 | 8 |
| gi/visibility_reuse | bool | false |
| gi/fresh_lo | bool | false |
| gi/blocked_keeps_coverage | bool | true |
| gi/confidence_ramp | bool | false |
| gi/ramp_m_min | int 1..255 | 4 |
| gi/ramp_frames | int 1..4096 | 64 |
| gi/revalidate | enum off,budget,share | off |
| gi/revalidate_period | int 2..16 | 3 |
| gi/revalidate_share | int 1..1024 | 8 |
| gi/revalidate_threshold | int 1..1000 (percent) | 50 |
| gi/revalidate_cut_m | int 0..16 | 1 |
| gi/m_light | int 1..32 | 2 |
| gi/m_light_frames | int 0..64 | 4 |
| gi/own_sampler | bool | false |
| gi/deterministic | bool | false |
| gi/dump_path | string | "" |
| gi/denoise | enum off,r2 | r2 |
| gi/denoise_history_cap | int 1..255 | 4 |
| gi/trace_all_tiles | bool | true |
| gi/confidence | enum frames,frames_and_m | frames |
| gi/replay_seed | bool | false |
| gi/event_reshade | bool | false |
| gi/reshade_budget | int 0..100 (percent of trace pixels) | 25 |
| gi/event_m_cap | int 0..256 | 0 |
| gi/jump_m_cap | int 0..256 | 0 |
| gi/event_reach_scale | float 0..64 | 2.0 |
| gi/event_solid_angle | float 0..4π (steradians) | 0.05 |
| gi/spatial_hold | enum off,uncapped | uncapped |
| gi/spatial_hold_frames | int 1..256 | 40 |
| gi/sky_reread | bool | false |
| gi/fold_k | int 0..64 (0 off; 1 to 3 read as 4; other values round down to a power of two, 4 to 64) | 0 |

The numeric defaults other than the bools are the ADR values. They have no effect while the matching enabled
setting is off. Every description starts with "Experimental.". Godot shows project-setting help from
doc/classes/ProjectSettings.xml (core), which must not name the module, so the descriptions are kept in
restir_settings.cpp and reach the probe status only. The editor settings page does not show them.

## Core include shim (`modules/gi_restir/restir_shim.py`)

- SCsub asks restir_shim.write_shim for the core include shim before it adds sources.
- Primary path: shaders/restir_core_inc.glsl (generated, git-ignored) holds one #include per core file, each path
  computed with os.path.relpath from the module shader folder to
  servers/rendering/renderer_rd/shaders/environment/. Before writing, each path is checked to resolve, as
  glsl_builders.py resolves it, to the same file. A path that does not resolve switches to the fallback.
- Drives: glsl_builders.py resolves includes with os.path.relpath from the engine build folder, and relpath cannot
  cross drives on Windows. If the module shader folder and the build folder are on different drives, write_shim raises
  ShimError with a message naming both folders and SCsub stops the build (exit 255). The fallback is not tried in that
  case, because its copies live in the module folder and have the same problem.
- Fallback: the core files and their nested includes are copied into shaders/restir_core/ (git-ignored) and the shim
  includes the copies. Every SCons run reads each core file and rewrites its copy when the bytes differ, so a stale
  or edited copy is healed on every SCons run. The fallback refuses an include outside the core folder.
- Dependencies: the gen header of the placeholder shader depends on the shim, glsl_builders.py and the transitive
  include closure of the core files (CORE_INCLUDES and EXTRA_INCLUDES in restir_shim.py; nested includes included,
  thirdparty skipped), for the shim and the fallback.
- Builder: SCsub uses RD_GLSL, guarded by "RD_GLSL" in env["BUILDERS"] like the core shader SCsubs. ADR 7.2 names
  GLSL_HEADER; that line is superseded.
- Placeholder shader: shaders/restir_shim_check.glsl includes the shim so the engine build proves the includes
  resolve. It is never compiled at run time. restir_probe.cpp includes its generated header, so the header is built in
  every build that builds the module (including custom_modules builds).
- SCsub loads restir_shim.py by file path (importlib), not through sys.path.

### Shade include contract (ray_scene_hit_shade_inc.glsl)

- In CORE_INCLUDES since GI's review of 14 Nov 2026, listed after ray_scene_inc.glsl. The file has no #include of its own;
  everything it uses comes from the file that includes it, which must supply, before the include:
  - the four macros RAY_SCENE_SHADE_MAX_DISTANCE, RAY_SCENE_SHADE_BOUNCE_ENERGY, RAY_SCENE_SHADE_EMISSION_EXPOSURE and
    RAY_SCENE_SHADE_MAX_RADIANCE (ddgi_trace.glsl sets them to ddgi.max_distance, params.bounce_energy,
    params.emission_exposure and DDGI_MAX_RADIANCE);
  - ray_scene_inc.glsl: RaySceneHit, ray_scene_hit_material, ray_scene_hit_normal, ray_scene.camera_to_anchor and
    gi_hit_direct_light (one #define to ray_scene_direct_light at the end of ray_scene_inc.glsl);
  - ddgi_inc.glsl (ddgi_sample_direction, DDGI_NO_VIEW_LIMIT) with DDGI_SET, DDGI_UBO_BINDING and DDGI_SAMPLING defined,
    and the probe, irradiance and distance textures and linear sampler bound (restir_gi.glsl does this under RESTIR_RAYS in
    FINAL, SOURCE and VALIDATE, the modes that shade, and binds the sky radiance and sampler there for its own
    rs_sky_radiance); shade_hit calls
    ddgi_sample_direction (view limit hit.t * 0.5) at run time when the bounce energy is above 0, but the include is
    needed at compile time whatever the energy;
  - under MODE_SURFEL_TRACE only: ddgi.levels[0].spacing, the globals origin_surfel_position and origin_surfel_normal,
    and surfel_inc.glsl and surfel_gather_inc.glsl (surfel_lookup, surfel_eval_irradiance, surfel_blend,
    SURFEL_INVALID).
- The import seed (gi/replay_seed) comes from rtgi_anchor_pixel and rtgi_shade_key in rtgi_inc.glsl, the same helpers R2's
  MODE_RTGI uses, so the imported tag is R2's seed at that pixel. The engine code notes hold the replay contract.
- The module never defines MODE_SURFEL_TRACE, so it needs neither surfel include nor the origin_surfel_* globals.
- A module shader that shades hits defines the four macros, includes restir_core_ray_scene_inc.glsl and
  restir_core_ddgi_inc.glsl, then restir_core_ray_scene_hit_shade_inc.glsl.
- restir_core_inc.glsl now includes the shade file with none of the above defined. Only the placeholder shader includes it,
  and the placeholder is never compiled, so the build still only proves that the paths resolve.

## Build cases

- a: module absent (same engine commit, no module files).
- b: in tree, module_gi_restir_enabled=no.
- c: in tree, module_gi_restir_enabled at its default (yes).
- d: a custom_modules= folder outside the tree with a copy of this module (identical sources).
- e: D3D12. The module has no hardware variant and no D3D12-specific source.

## Probe

- Engine singleton GiRestirProbe, method get_status. Keys: module, rtgi_registered, rtgi_name, rtgi_active,
  rtgi_status, settings (name -> value, description, internal, in
  declared order).
- Game probe: opencw/tools/gi/GiRestirProbe.cs and gi_restir_probe.tscn (headless).

## Backend contracts

- release(nullptr) is called at shutdown only by this module's uninitialise (release_all, then unregister_all); the
  renderer never calls release_all. Backends must make release(nullptr) idempotent and must not touch the device after
  the first call.
- is_active() runs on the render thread every frame: no allocation, no GPU work, no locks.
- gi/deterministic and gi/dump_path are internal settings: they do not appear in the editor.

## Provenance

- No paper, SDK or third-party code was read or used for the module skeleton. Sources consulted: the core hook headers
  (servers/rendering/renderer_rd/environment/gi_module_hooks.h and .cpp), SConstruct module discovery,
  glsl_builders.py include resolution, project_settings.h, and the module precedents modules/gi_hook_probe and
  modules/lightmapper_rd.

## ReSTIR GI core (gi backend "restir_gi")

Files: restir_gi_core.h/.cpp (resources, pipelines, pass dispatch), restir_gi.h/.cpp (backend: refusals, history cuts,
epoch handling, status), restir_test.h (probe options and per-frame records), shaders/restir_gi_inc.glsl and
shaders/restir_gi.glsl (modes CLEAR, TEMPORAL, SPATIAL, FINAL, SOURCE; SW/HW variants selected by the ray scene).

Layout: the reservoir is 48 B per pixel stored as a 3-layer RGBA32UI 2D array, two sets ping-pong. Words: xs (3 float), ns
(oct16); Lo (3 half), flags (16 bit); W (float); M (16 bit), age (16 bit); receiver depth, receiver normal (oct16), epoch
tag, rng tag. The RestirFrameUBO is 320 bytes (static_assert).

Pass chain per frame: temporal (candidate + reprojected history, set A -> B), spatial (1 or 2 passes, B -> A, A -> B), final
(radiance, coverage, debug, per-group statistics). The final set becomes next frame's history by index, no copy.

Geometry: every receiver is described by its ray origin, pos + n * offset, with the offset sized to the position error of the depth-derived
position: rtgi/origin_scale (default 2) times (2^-23 * (4 * |view position| + 4 * max |anchor position|) + gap / 256), where gap is the larger of the two
smaller-gap neighbour distances in the geometry texture; rtgi/origin_floor (default 0) is a floor of origin_floor * |view position|. n is the geometric
normal from those neighbours when it agrees with the shading normal (cosine above 0.7), else the shading normal. Core R2 uses the same rule with full-resolution
neighbours, so the two origins agree to about the footprint term, which the module measures at trace resolution (up to resolution_scale times larger).
Limit, in plain words: huge flat triangles (hundreds of metres across) seen up close may self-shadow in R2's bounce, because the depth buffer cannot place
the receiver more exactly than the rasteriser did on such triangles (measured 1e-4 of the view distance on a 6 km two-triangle plane, against 8e-6 on 50 m tiles). Directions, cosines, Jacobians and visibility rays use the origin, so a candidate's
density is exactly cos / pi in the direction measure around that origin. S4F: the fresh candidate weight is W = 1 / pdf,
the pdf of the candidate record (word 1.z, solid angle at the receiver). A candidate whose pdf is not finite and positive
is a refusal: counted in the status (pdf_refused) and treated as no candidate. S4 used pi / cos of the reconstructed
direction, valid only for cosine-drawn sources.

Modes: B1 (m = M_i / sum M, W clamp 20 x 3x3 median, final visibility ray), B2 (B1 + visibility on every reuse), U1
(pairwise MIS, symmetric accept rule, visibility both ways); B2k (B2 keeping failed techniques' M) is in the "B2k, keep M"
section below. A neighbour whose sample is invalid for the receiver
(Jacobian, distance, cosine) stays in the technique set with weight 0; dropping it changes the canonical share per sample
and biases the result (found by the S4 region-sum gate: +1 % to +15 % before the fix).

Stored M (S4F, F1; per-mode since S4F2). U1: M_out = min(M of the canonical + M of every present technique, m_max); it
never depends on a stored sample. A technique whose sample fails the shift keeps weight 0 and keeps its M in U1. B1 and B2
renormalise: the weight denominator and the stored M are the sum of M over the techniques that contribute to this combine
(the canonical when it has a sample, plus neighbours whose sample shifts validly), capped at m_max; a present technique whose
sample fails (shift, vis_fail limit, 0x100, empty sample) adds neither M nor weight. Using the present sum in B1 made every
combine lose the share of M whose sample failed, and the loss compounded through the stored W (S4F T1 cornell 0.607; chained
toy in RestirMRuleTests: present-sum B1 0.44, renormalised 0.97 of truth). A technique is present when the receiver-side and scene-side tests pass (frame, plane, normal,
class, epoch, cut, dirty box); the sample-side states below keep it present with weight 0 (F2): the vis_fail limit, a held
vis_state 0x100 (the S4 reduction of M to 1 is gone), a history reservoir with M > 0 and an empty sample, and in the
spatial pass a neighbour with M > 0 and an empty sample. A reservoir with M = 0 is absent (nothing to count). Why: with
an on-only M, the stored M correlates with the stored sample, and the pairwise canonical share and B1 normalisation then
mix different M in one MIS partition (chained toy in tests/Lighting.Tests/RestirMRuleTests.cs: +22.9 % on-only, +0.07 %
present).

Coverage: clamp(M / N_target) for every pixel that holds a valid reservoir, including valid samples whose estimate is 0.
Coverage is 0 only for no sample, a non-finite W, or a final-visibility block. ADR 5.10 says "0 where W = 0"; that rule
sends every exactly-black sample to the fallback layer and biased the region mean upwards (+16 % in the cornell room).

Test source (restir/gi/own_sampler): the module's own SOURCE pass overwrites core's candidates with one cosine ray per
pixel, Lo = emission + albedo * ray_scene_direct_light (no DDGI bounce), sky = constant colour from the probe. Default off.

Statistics: the final pass writes 16 floats per 8x8 group (serial shared-memory sums, no float atomics); the CPU sums the
groups in double. The info texture holds temporal and spatial reject reasons (0 none, 1 frame, 2 plane, 3 normal, 4
class, 5 epoch, 6 dirty, 7 jacobian, 8 distance, 9 cut, 10 no sample, 11 visibility).

Debug views: the viewport enum values 76..82 (VIEWPORT_DEBUG_DRAW_GI_BACKEND_0..6) map to module views 57..63. ADR 8.2
names 57..63; those numbers are already used by the RTGI views in the engine.

Invalidation (S5, ADR 5.9): see "Lane S5" below. Consecutive visibility failures above 2, or a held 0x100 state,
give the history technique weight 0 with its M counted (F2).

The module constructs its shader objects lazily on the render thread (ShaderRD needs the rendering device).

Replay and synthetic harness (restir_harness.cpp, probe methods request_capture, get_capture, run_case, case_ready,
get_case_result, read_case_file, write_case_file): the module settings `restir/gi/dump_path` and `dump_after` write the
inputs of one live frame (geometry, previous geometry, candidates, the previous reservoir set, visibility state, frame
parameters) as a Variant dictionary; a MODE_CAPTURE pass copies the core's textures into module-owned ones first because
the core textures lack the copy-from usage. run_case rebuilds the textures from such a dictionary (live or hand built),
runs the pass chain with the ray scene off, and returns radiance, coverage, debug, info, the reservoir set, SHA-256 of each
and the statistics. `restir/gi/deterministic` fixes the random frame and seed and makes the spatial offsets a fixed spiral
(angle k * 2.39996 + 1.3 * pass_index, length radius * max(sqrt((k + 0.5) / count), 0.15)). pass_index is 0 for the
temporal pass, 1 and 2 for the spatial passes; the offsets and the selection random dimension (candidate + 8 * pass_index)
use it, so an external reference has to use pass_index 1 for the first spatial pass.

The first spatial pass of a deterministic frame is a pure function of its inputs, so the synthetic cases in the game probe
(modes arith, viewcases) compare it with a double-precision CPU reference.

Truncation counters (S4F, F3) and clamp share (F4): the temporal and spatial passes write PS_STRIDE = 32 floats per 8x8
group per pass into a pass statistics buffer (4 rows: temporal, spatial 1, spatial 2, validate; cleared every frame). Per frame:
techniques present, techniques kept at weight 0 by a sample-side state, neighbours tested (valid sample reaching the shift
test), rejected by d_min, by cos_min, by the |J| range, the energy of the tested candidates plus the canonical
(M * p-hat * W * jac_raw) and the energy of the rejected ones, pdf refusals. jac_raw is the Jacobian without the range test,
clamped to [0, 1000], with the cosine at the sample side at least 1e-3. Status fields (stats dictionary):
truncated_candidate_share, truncated_energy_share, clamp_share (CLAMPED pixels over geometry pixels), pdf_refused,
techniques_sample_unusable, lo_refused, no_canonical, temporal_shift_tested, temporal_shift_rejected, spatial_combines (pixels that ran a spatial combine with at least one neighbour; 0 with
spatial_neighbors 0). Probe options d_min, cos_min, jac_max, vis_fail_limit override the limits for the truncation-off
measurements.

S4F2 additions. B2 (per-reuse visibility): a neighbour that shifts validly but is blocked at the receiver adds neither M nor weight (its
on flag is cleared); B1 without visibility reuse and U1 keep the shift-valid set. A refused fresh candidate (non-finite lo, or pdf <= 0 or
inf) keeps m = 1 with on = false and is counted (lo_refused, pdf_refused); a canonical that is not on contributes no M in B1 and B2 and its
M = 1 in U1. The pass statistics are written and reduced only when the probe readback is on (flag bit 128); the energy sums skip NaN and
Inf. no_canonical counts temporal pixels whose canonical is not on (thinned or refused).

Test options (probe, restir_test_options): w_clamp overrides the W clamp factor in the final pass (diagnostic); freeze makes the module return its last
radiance and coverage unchanged after a final capture, so a per-frame dump and the module readback describe the same frame;
request_final_capture asks the next frame to keep the final reservoir layers, the view-space geometry and the debug texture after the final
pass (get_final_capture returns them; the blocker scene's view 62 oracle uses it). Spatial reuse is switched off with spatial_neighbors = 0
(spatial_passes is clamped to 1..2); spatial_combines is 0 then.

## Invalidation and R2 integration (`modules/gi_restir/restir_epochs.cpp`)

Planner: restir_epochs.h/.cpp, restir_plan_epochs() decides per frame from the history flags, the previous and current GIEpochs::Snapshot and the dirty feed
(RtgiFrameInputs.dirty_source, the GIEpochs singleton core sets; no core accessor was added). Order: no history (FIRST), core history_cut (REQUEST; camera cut,
resize and backend switch reach the module only through it), hardware change (BACKEND), a Fold change (FOLD), origin epoch change (ORIGIN), world epoch change (WORLD),
then a scene epoch change. The scene epoch moves on every ray scene update in which the scene tracker's signature changed (an instance added, removed or moved, its mesh
rebuilt, its layer mask or material entries changed; a mesh rewritten every frame moves it on every update), so a scene
change is not a cut by itself. The backend holds the ray scene's dirty feed (RayScene::FEED_MODULE, taken in resample, given back when its last state is released) and,
when the scene epoch differs from the one stored with the history, asks GIEpochs::get_dirty_boxes(stored scene epoch) through RtgiFrameInputs.dirty_source. The planner
then reads: no live feed, SCENE_NO_FEED and a full cut; DIRTY_NONE (a change with no geometry behind it, a material entry for one), no cut; DIRTY_OVERFLOW or more than
RESTIR_DIRTY_MAX boxes, SCENE_OVERFLOW and a full cut; DIRTY_BOXES with no box or a box that is not finite or has a negative size, SCENE_UNLOCALISED and a full cut;
otherwise the boxes go to the dirty test below and nothing else is cut. A world epoch change (no instance of the last ray scene is left) is a full cut whatever the
feed says. Boxes are world space; the planner subtracts RayScene::get_anchor() to get the anchor-relative space of the geometry. The feed steps the scene epoch itself
when a frame has changes or a raw overflow and the scene tracker did not step it in that update (RayScene::feed_frame_needs_epoch: a hardware
usability flip is the one change the feed sees and the tracker's hash does not), because get_dirty_boxes returns only frames newer than the reader's epoch. State counters: cut_counts per reason,
dirty_frames; status keys cut_reason, dirty_boxes, dirty_feed, scene_changed, light_changed, sky_changed, origin_changed, world_changed, light_cap, light_frames_left.
What makes a box: the feed compares, per instance, the world transform, the mesh and its build serial, hardware usability, and a state word built from the layer mask
and each surface's material flags (shadow casting included) and coverage (RayScene::feed_surface_state). So a change of who a mesh shadows or how much it lets through
dirties its bounds like a move does. A change of albedo or emission alone (the game animating an emission scale) moves the scene epoch with no box: history is kept,
as it was before the scene epoch moved at all, and the re-validation rays are what follow it. When restir/gi/enabled is off, RTGI releases the backend's state for the
buffers it is rendering (RtgiPass::_process), which frees the history textures and, with the last state, gives the feed back; switching the module on again starts
from a first-frame cut.

Dirty test (restir_gi.glsl rs_dirty_hit): per reprojected history pixel with a stored sample, the slab test of the segment from the previous receiver surface point
q.pos to the stored sample point x_s (to q.pos + dir * sky_distance for a sky sample) against every box grown by dirty_margin 0.01 m. A hit gives reject code 6 (REJ_DIRTY)
with the history unused: M = 0 from it and a fresh canonical only; the pixel keeps INFO_HAD_HISTORY. Epoch tags: the stored tag is compared with prev_epoch_tag (the
previous frame epoch_tag), reject code 5.

Light epoch (Experimental): a light epoch change sets m_light_frames frames in which m_temporal_cap and m_max are capped at m_light, so the stored M is at most m_light
for exactly that many frames (defaults 2 and 4). It is a soft cap, not a cut.

Sky epoch: nothing is cleared. The sky radiance is core-owned (ddgi_trace.glsl sky_radiance_in, set 2 and push constants); the ray scene carries no sky, so a module
cannot re-read the sky by direction. A MISS_SKY sample therefore keeps its stored Lo until it is replaced; this is measured lag, not a re-read.

## Port onto the R2 line

- Rays: ray_scene_trace with DI-1's seed argument. Each call site hashes the pixel, the frame and its own odd salt.
- Backface: a hit is shaded when it faces the ray, or when its material is two-sided or a blend material, as in ddgi_trace.glsl shade_hit.
- Hardware ray scene: when RayScene::uses_hardware() is true the module uses VARIANT_HW (RAY_SCENE_HW and GL_EXT_ray_query, the ray scene's own bindings
  through append_uniforms). If that variant or one of its pipelines cannot be made, the module refuses the frame and turns the hardware backend off for the ray
  scene, as DDGI does.
- No cloud shadow and no alpha ray scene on this line; the shade passes bind set 2 bindings 0 and 3 to 8 only.
- Output route: only gi/denoise = r2 is used by RTGI. A frame with gi/denoise off is counted as a refusal and RTGI runs on its own candidates.
- Tiles: wants_all_tiles() returns gi/trace_all_tiles, and while it is true R2 traces every tile. With it off, R2 picks its tiles and the module keeps
  state only in the traced ones (change plan commit 9, "Traced tiles only" below). The default stays on.
- Per-view release: R2's RtgiBuffers::free_data calls release(p_buffers) when the render buffers are cleaned up (destructor, configure,
  configure_for_reflections).

## Replay (gi/replay_seed, default off)

- R2 side: wants_replay() returns gi/replay_seed. R2 then sets RTGI_FRAME_FLAG_REPLAY (512) in the trace UBO's flags only. MODE_RTGI
  defines RAY_SCENE_REPLAY and sets ray_scene_replay from that bit in main, so the hit light pick is the id-keyed race and the pdf word is
  the folded density p0(w) + p0(m(w)) whenever the mirror path is on and its 0.999 test passes (both terms floored at 1e-4, as the CPU
  toy's RestirFold.Pdf). With the bit clear the pick and the pdf word are today's.
- Module side: FINAL, SOURCE and VALIDATE define RAY_SCENE_REPLAY, and rs_shade_setup sets ray_scene_replay from F_REPLAY before
  every replay shade, so a re-shade takes the same id-keyed race, area-light point and shadow seeds as R2's candidate shade. The
  non-replay paths never call rs_shade_setup, so the global stays false there and their pick and seeds are today's.
- Seed at import: s = ray_scene_hash(pixel.y * 65537 + pixel.x + rf.frame * 6271) with pixel = min(trace_pixel * scale + scale / 2,
  screen_size - 1), R2's expression in MODE_RTGI. rf.frame is R2's frame_index on this line. It goes into the reservoir's tag word. Under
  gi/own_sampler, SOURCE writes its own shade seed into the candidate's tag and the import keeps it; SOURCE's trace then takes the same
  seed. RestirFrame grows by one uvec4 (screen_size, trace_scale, a spare word): 400 bytes.
- Shared shade: the module includes ray_scene_hit_shade_inc.glsl with the four macros bound to globals that rs_shade_setup fills: DDGI
  max distance, bounce energy and emission exposure when DDGI shading is on and the own sampler off, else rf.sky_distance, no bounce
  and rf.emission_exposure. Max radiance is RS_LO_CLAMP (60000, the same value as DDGI_MAX_RADIANCE). The old copies (rs_fresh_lo and
  SOURCE's) stay for replay off, so its output is unchanged.
- VALIDATE in replay mode: a short re-hit from x_s + n_s * eps along -n_s, length 2 eps, eps = 0.02 + 0.002 * d (d from the previous
  receiver), with the stored seed. The hit is the stored surface only if |t - eps| < 0.5 eps and its facing normal has dot > 0.99 with
  n_s. Then it is shaded from the previous receiver with t = |x_s - o_prev| and the stored seed, and the new Lo, the re-hit's facing
  normal and the backface bit (shade alpha below 0) are written. A miss or a mismatch writes Lo = 0 and keeps M and the flags, counted
  as rehit_miss or rehit_mismatch (pass stats row 3, slots 16 and 17). A re-shade that is not finite writes Lo = 0 the same way,
  uncounted. No 10x limit and no change cut. Sky samples are left alone.
  The share selection draws with rf.frame in deterministic mode too (rs_rand_frame).
- FINAL with gi/fresh_lo in replay mode traces from the receiver with the sample's stored seed and shades through the shared shade.
- Pass stats: PS_STRIDE 32 and four rows (TEMPORAL, two SPATIAL, VALIDATE). rs_stats_store takes the row.
- Set gi/replay_seed before a run. A change mid-run cuts no history: samples imported before it keep R2's pixel and frame tag,
  which a replay re-shade then reads as a seed, so they re-shade with other draws until the history turns over.
- wants_replay() must come from a setting: R2 reads it before begin_frame, to set the trace flag, so it cannot depend on anything
  the module decides inside the frame.
- The mirror floor: MODE_RTGI floors the mirrored term at 1e-4/π (about 3.2e-5, ddgi_trace.glsl:520). Where the true p0(m(w)) is 0
  (m(w) below the shading hemisphere) the floor adds density that is not there. That happens only at near-grazing w, where the pdf
  word can read up to about 2x p0(w); elsewhere it is negligible (from the R2 side's review of this change).
- direct_shadows.glsl does not define RAY_SCENE_REPLAY, so DI-1's picks stay index-keyed. ReSTIR DI must define it (and set the
  global) if it ever wants to replay a DI-1 pick.

## Events (gi/event_reshade, default off)

Design 2.8, 8.1 rows 5-8, 8.3 row 4; change plan commit 5.

- Needs gi/replay_seed. begin_frame refuses the frame ("event_reshade needs replay_seed"), and R2 then runs plain, when it is off;
  also without a ready ray scene ("event_reshade needs the ray scene"), without valid DDGI shading inputs ("event_reshade
  needs the DDGI shading inputs", so a re-shade never runs at sky distance with no bounce) and with gi/revalidate = budget
  ("event_reshade does not run with revalidate budget"), because that form drops the frame's candidates and the event path
  keeps them. off and share are allowed.
  replay_seed on with event_reshade off is commit 4 exactly.
- Input. R2's _read_light_changes reads the tracker ring once per frame with its per-buffers cursor and keeps an anchor-relative
  copy (RayScene::get_anchor subtracted in real_t, then the module stores float) in RtgiBuffers::backend_events, so it outlives the
  call. RtgiFrameInputs gains events and event_count (that copy, at most 64), events_lost (this frame's growth only; the buffers'
  counter stays cumulative), directional_jump (RtgiRestarts' jumps > 0, R2's own sun and moon rule, repeats excluded) and
  restart_quiet_frames (rtgi/restart_quiet_frames). The module keeps no cursor. On a view's first frame no events arrive.
- RtgiFrameInputs::events is valid only during the backend call: the next frame's _read_light_changes refills the vector, so
  the planner copies what it needs into the event UBO. RESTIR_EVENT_MAX >= DDGILightTracker::MAX_EVENTS is a static_assert,
  so the copy never drops a delivered event.
- One tracker ring (64 slots, no view tag) serves every view, each RtgiBuffers with its own cursor. A burst of more than 64
  events between two reads of this view, written by any view's update, makes events_lost positive here, and the frame is a
  jump frame (every receiver capped) even with no change near this view. Conservative, not wrong; fires are rate-limited to
  one per light per 8 frames. If a second live view (SubViewport, probe capture) ever runs RTGI, a region test on lost events
  would avoid the extra jump frames (from the R2 side's review of this change).
- epochs.light lags the events by one frame: the epoch snapshot is taken (render_forward_clustered.cpp) before
  _read_light_changes bumps SOURCE_LIGHT, so on frame N the module sees N's events and jump, and the light epoch changes on
  N+1. With events on, m_light_frames is 0 and nothing depends on it; with events off, the epoch path caps from N+1 as
  before.
- The light epoch: with events on, the planner gets m_light_frames 0 and the carried light_soft_frames is zeroed, so light_cap is
  false and gi/m_light never caps. The light change is still reported (light_changed). The old soft_hold_frames output no longer
  exists on this line (6d6fa7157), so "the soft hold is off" has nothing more to turn off.
- R2's own restarts (soft cut, restart spheres) are not touched here; they stop under the module-output bit (commit 10). Until
  then an event frame restarts in R2 as well.
- The event UBO, set 0 binding 26, std140, 2368 bytes since the SPATIAL hold (1328 before it, without held[64] and hold),
  declared in TEMPORAL, SPATIAL and VALIDATE and always bound (zeros when
  off): counts = (sphere count, event count, event_m_cap, jump_m_cap); params = (event_reach_scale, event_solid_angle, 0, 0);
  gates = (quiet frames, budget B, jump frame 0 or 1, VALIDATE selection 0 none, 1 event, 2 all); sphere[64] = (centre, reach) in
  metres, anchor-relative, reach = the tracker's half; since_last[16] holds sphere i at [i >> 2][i & 3]. The first event-count
  spheres are events; slots after them are for dirty-box spheres (centre, half diagonal, since_last UINT32_MAX), none on this line:
  the planner's dirty boxes go to the shader's own dirty test (rs_dirty_hit), not to the event UBO. RestirFrame stays 400 bytes. The CPU side packs it in
  restir_events.cpp (restir_plan_events), with no RD dependency, so the doctests (tests/test_restir_events.h) read the same rules.
- F_EVENTS is rf.flags bit 24. Bits 19 and 22 stay free for Fold and B2k. The jump frame lives in the UBO, not in a flag.
- Sample test (VALIDATE): the stored x_s lies in any event sphere, old or new, first fire or repeat: |x_s - c|² <= R², raw reach.
  Sky samples never.
- Receiver cap test (TEMPORAL, SPATIAL): over the capping spheres (events with since_last >= quiet frames, UINT32_MAX always, every
  event at the default quiet 0; plus dirty spheres), d = |x - c| < event_reach_scale R, or the solid angle 2π(1 - sqrt(1 - (R/d)²)),
  written 2π k²/(1 + sqrt(1 - k²)) with k = R/d for precision, exceeds event_solid_angle. d <= R or d < 1e-6 counts as 4π.
  Visibility is ignored (over-caps, noise not bias).
- TEMPORAL: after m_h = min(h.m, m_temporal_cap) (so after the rate scaling and the confidence ramp), a receiver r.pos that passes
  the cap test takes m_h = min(m_h, event_m_cap); on a jump frame every receiver takes m_h = min(m_h, jump_m_cap). The history stays
  present; M 0 is weight 0. No reject code is added.
- SPATIAL: when the receiver passes the cap test, each neighbour that does not pass it at its own position q.pos takes
  M = min(min(n.m, m_max), event_m_cap); neighbours inside the capped region keep their M (design 2.8 "neighbours inside the capped
  region keep their M", Q10 H1). This is the event frame; the SPATIAL hold below carries it over later frames. The jump cap
  is TEMPORAL only.
- Jump frame: directional_jump or events_lost > 0. Candidates are kept (F_REVAL_FRAME is never set).
  jump_m_cap 0: restart from the fresh candidates and spatial reuse, no event or all re-shade. jump_m_cap > 0: VALIDATE re-shades
  every stored valid non-sky sample first (selection all: no sphere test, budget or cap-0 skip). A sky epoch jump (gi/sky_reread,
  decided on the GPU, see "Sky re-read") is not a jump frame in this sense: it applies only the TEMPORAL cap at jump_m_cap, with no
  VALIDATE re-shade, no hold clear, and no change to status jump_frame.
- VALIDATE event selection, on the previous history set in place, before TEMPORAL. It runs when events are on, the history is used,
  the ray scene is there and the frame has events (not a jump frame). Early outs as replay: M 0, not RF_VALID, an old epoch tag, an
  invalid previous receiver, sky, |x_s - o_prev| < 1e-3. No tile skip and no skip on this frame's NO_CANDIDATE. Cap-0 skip: with
  event_m_cap 0 a flagged sample whose previous receiver q.pos passes the cap test is not re-shaded, since its history takes no part.
  The re-shade is commit 4's rs_validate_replay unchanged. A share pick (gi/revalidate share) in the same dispatch re-shades as
  before; a pixel picked by both is re-shaded once.
- Budget: B = floor(trace_w trace_h reshade_budget / 100). Order: key = rs_rand_frame(px, 200) (dimension 200 is unused; share uses
  40, combine below 64, spatial and compat 96 to 176), new every frame. A count dispatch of the same VALIDATE shader (its own
  RestirFrame UBO with pass_index 4) adds each flagged pixel to one of 256 bins, bin = min(key 256, 255), in a 1 KB storage buffer
  at set 0 binding 28 (cleared each frame). The main dispatch takes, per work group, the cut t = the first bin at which the running
  sum would pass B, and re-shades flagged pixels with bin < t. Taken <= B always, and taken = flagged when flagged <= B; otherwise it
  can fall short of B by less than one bin's count. A flagged sample not taken keeps its stale Lo this frame (reshade_overflow).
- Order with events on: plan epochs (m_light_frames 0), plan events, write the event UBO; SOURCE if own sampler; VALIDATE count,
  VALIDATE; TEMPORAL; SPATIAL; FINAL; PACK.
- Counters. Status, always: event_reshade, events_read (events delivered this frame, also with events off), events_lost (this
  frame's growth), jump_frame, event_capping_spheres, event_validate (0, 1, 2), reshade_budget_pixels, and the backend-lifetime
  totals event_frames_total (frames with events_read > 0), events_read_total, event_validate_frames_total and
  event_capping_total, because a lab reads status once, after its last frame. Status stats (probe readback
  only, F_PROBE_STATS; production has no GPU readback): reshaded (event or all selections re-shaded, pass stats row 3 slot 19; share
  picks are not counted), reshade_overflow (row 3 slot 20, flagged not taken and not share-picked, plus reshade_edge_stale),
  reshade_edge_stale (row 0 slot 21: event_m_cap 0, the history sample in an event sphere, the previous receiver capped so VALIDATE
  skipped it, the current receiver not capped), event_capped (row 0 slot 18: receivers whose history M the event cap lowered).
  PS_STRIDE stays 32.
- Identity with event_reshade off: the event UBO is zeros, F_EVENTS is clear, the VALIDATE dispatch condition is today's and no count
  dispatch runs, so TEMPORAL, SPATIAL and VALIDATE do today's arithmetic in today's order (SPIR-V of those three changes; every other
  module pass, every DDGI trace variant and both direct-shadow variants are byte-identical, spirv_c5.py).
- The harness carries the packed UBO in a captured case as "events" when F_EVENTS is set.
- Later item (14 Nov): R2's tracker fires two events per lamp change, the old and the new state
  (`ddgi_light_tracker.cpp` `fire` twice), and R2's select merges them into one sphere for a light that doesn't move. The module
  takes both as capping spheres, so a stationary flicker reads events_read 2 and event_capping_spheres 2 per changed frame
  (flicker lab, 2f01e4725: 32 frames, 64 events). The M cap is a min, so the output is the same, but the count dispatch and the
  per-workgroup cut test each sphere twice. Dropping exact duplicate spheres in the event plan is left for later.

## SPATIAL hold (gi/spatial_hold, default uncapped)

Design 2.8(b), change plan row 6. It runs only with the event path on (gi/event_reshade and gi/replay_seed), so with events off
nothing changes.
- The ring. Each view's RestirGiState keeps a RestirHoldRing of 64 entries: centre in world space (the event centre plus the ray
  scene anchor, so an anchor move doesn't shift a held sphere), reach, age. Ages stay on the CPU; the GPU gets centres and reaches
  only.
- The update, once per frame after the event plan (restir_hold_update): every entry ages by 1 and leaves at age
  spatial_hold_frames, so a sphere is held on its event frame and the next spatial_hold_frames - 1 frames (40 frames at the
  default). Then each of the frame's capping spheres, the first fires (since_last >= restart_quiet_frames, the same test as the
  cap), either refreshes the first entry whose sphere holds its centre and whose reach is at least its own (age back to 0, no new
  slot), or takes a new slot. A full ring replaces its oldest entry (the highest age, the lowest index on a tie), counted as
  hold_dropped. R2's old-and-new pair for one change lands as one entry and one refresh. Dirty boxes are not in the event list
  yet, so they are not held either.
- The ring is cleared when the hold is off, when events are off, on a frame without history (a cut), and on a jump frame; the
  frame's own first fires then go in. spatial_hold_frames 0 cannot be set (range 1..256); the function clears the ring for it.
- The GPU side. The event UBO (set 0 binding 26) gains vec4 held[64] (anchor-relative centre, reach) and uvec4 hold (x = count),
  2368 bytes in all (static_assert in restir_events.h). F_SPATIAL_HOLD (bit 25) is set when the hold is on and the ring is not
  empty. rs_hold_caps is the event cap test (reach scale and solid angle) over the held spheres, without a first-fire check
  (every held sphere was one).
- SPATIAL: the receiver counts as capped when it passes the event cap or the hold test; a neighbour q then takes
  M = min(min(n.m, m_max), event_m_cap) unless q passes the event cap or the hold test itself ("neighbours inside the capped
  region keep their M"), on the event frame and after it alike. With the hold off this is commit 5's rule. The event list stays
  in both tests because a first fire that refreshed a larger held sphere is not in the ring, and its own reach-scale and
  solid-angle region can reach past the larger sphere's. TEMPORAL and VALIDATE don't read the ring.
- The hold needs a ready ray scene: the event centres are relative to the ray scene's anchor, so without one the ring is cleared
  and nothing is stored.
- Counters. Status, always: spatial_hold, spatial_hold_frames, hold_fill (entries after this frame's update), hold_added,
  hold_refreshed, hold_expired, hold_dropped (this frame, the last view to run), and the backend-lifetime hold_frames_total
  (view-frames with a non-empty ring), hold_dropped_total and hold_fill_max, summed or maxed over every view's ring.
- Identity: with gi/spatial_hold off, or with no first fire since the last clear, F_SPATIAL_HOLD is clear and hold.x is 0, so
  SPATIAL does commit 5's arithmetic. The UBO grew, so the SPIR-V of every pass that declares it (TEMPORAL, SPATIAL, VALIDATE)
  changes; the other module passes and every DDGI and RTGI variant don't. A capture made before this commit carries a 1328-byte
  "events" blob, which the harness no longer loads.

## B2k, keep M (gi/mode B2k)

Change plan row 7, design 2.5 ("keep-M is not the default"; arm B2k in the CPU toy and every reference cell). B2k is B2 (the
same flags, F_VIS_REUSE included) plus F_KEEP_M (bit 22), RESTIR_MODE_B2K = 3 (mode_code 3, or 7 with fresh_lo; only the debug
view reads it).
- A Cand has a keep bit. In the B modes rs_combine adds a candidate's M to the total when it is on or kept; the weight loop
  still skips anything not on, so a kept candidate has zero weight and its M stays in the denominator (biased by
  -Σ m_i ∫ over what image_i can't reach, never brighter: G6.1(d)'s keep-M form).
- Kept under B2k: rs_fill_neighbour's early return when its dead reason is REJ_VIS or REJ_DIRTY (TEMPORAL's history
  visibility state over vis_fail_limit or flagged, and TEMPORAL's dirty history below), and the visibility test at the
  receiver that turns a B-mode candidate off. TEMPORAL's dirty reject of a valid stored sample goes down that unusable path
  with REJ_DIRTY instead of leaving the history out, so its M (after m_temporal_cap and the event cap) stays in.
- Not kept (renormalised as in B2): shift-limit failures (the Jacobian, distance and cosine limits, r_reason from
  rs_shift_jacobian); an invalid stored sample (RF_VALID clear, dead reason REJ_NO_SAMPLE, in TEMPORAL or SPATIAL), dirty or
  not; and candidates that never enter (out of frame, not similar, no stored sample, epoch). Tile flags arrive with commit 9.
  Keeping an invalid stored sample would carry M on an empty reservoir from frame to frame: a pixel with no canonical stores
  M = m_h and an empty sample, which would be kept again the next frame and never decay (commit 7 review, SHOULD-FIX 1). The
  design names visibility, dirty and history visibility only.
- RESTIR_F_KEEP_M is taken from the mode alone: _upload masks bit 22 out of the caller's flags, so a capture or caller can't
  turn keep-M on under B1 or B2.
- U1 is untouched: F_KEEP_M is never set with F_UNBIASED, and U1's total already counts every present candidate.
- Identity: with B1, B2 or U1, keep stays false, TEMPORAL takes the old dirty reject (the new dirty block can only run under
  F_KEEP_M), and the totals are the old ones. The Cand struct grew by one bool, so the SPIR-V of the passes that use it changes;
  their output must not.

## Sky re-read (gi/sky_reread, default off)

Design 2.9 (N3) and 8.1 row 10; change plan commit 8.
- The flag. F_SKY_REREAD is rf.flags bit 26 (67108864); bit 19 stays reserved for Fold. _upload sets it from
  RestirFrameParams::sky_reread alone and masks bit 26 out of the caller's flags (as bit 22 for B2k), so a capture or a caller
  can't set it. resample decides the effective flag before it plans the history cut: the setting is on, the ray scene is ready,
  the shading inputs are valid (shade_valid, the condition of F_SHADE_DDGI) with a DDGI UBO, and the frame's ray variant built its
  sky modes (RestirGiCore::is_sky_valid). run_frame repeats the test as a guard (VARIANT_NONE, no DDGI UBO, sky modes not
  built), so the harness replay (no rays) never runs it. With the own sampler and the constant test sky (F_REAL_SKY clear) the
  live sky is the test sky.
- What changes under the flag. A sample with RF_SKY (a MISS_SKY candidate) takes p̂ and radiance from
  rs_sky_radiance(normalize(x_s)), the lookup FINAL already made for fresh_lo.
  - TEMPORAL: the import checks R2's candidate radiance for finiteness as before, then sets the fresh sample's Lo from the live
    sky. A history sample gets its Lo from the live sky in rs_fill_neighbour after the early return for unusable samples, so a
    technique that takes no part costs no lookup.
  - SPATIAL: the pixel's own sample (M > 0, RF_VALID) and each usable neighbour the same way.
  - The looked-up radiance is cached in the candidate's Reservoir.lo (Cand.s), so rs_phat is unchanged and reads it at the
    receiver and, in U1, at the neighbour: one lookup per sky technique evaluated.
  - rs_store in the sky modes writes Lo = 0 for every RF_SKY sample, so a stored sky sample's Lo bits are zero, and under the flag
    nothing reads them.
  - FINAL: a sky sample with W > 0 takes its radiance from the live sky before the visibility block; fresh_lo's own sky lookup is
    skipped then, so it is still one lookup.
  - VALIDATE (gi/revalidate budget or share without gi/replay_seed): a sky sample whose visibility ray lands keeps its stored Lo
    (zero) and M, with no change cut, which would compare 0 with the live sky and cut every sky sample. A sky sample whose ray is
    blocked still loses M as before. The replay and event paths skipped sky samples already.
- Variants. Six new modes in restir_gi.glsl, PassMode 11 to 16: PASS_TEMPORAL_SKY, PASS_SPATIAL_SKY, PASS_SPATIAL_COMPAT_SKY,
  PASS_SPATIAL_WIDE_SKY and PASS_SPATIAL_COMPAT_WIDE_SKY (the plain mode's defines plus SKY_REREAD), and PASS_SKY_EPOCH
  (MODE_SKY_EPOCH). With RESTIR_RAYS, SKY_REREAD defines RS_SKY_REREAD.
  - Set 2 and the one include. The top of restir_gi.glsl defines RESTIR_SHADE (FINAL, SOURCE, VALIDATE) and RS_SKY_REREAD in two
    blocks, then one block under RESTIR_SHADE or RS_SKY_REREAD declares DDGI_SET 2, DDGI_UBO_BINDING 0, the DDGI sampling
    bindings 3 to 6 (RESTIR_SHADE only, with DDGI_SAMPLING), binding 7 sky_radiance (texture2DArray under
    USE_RADIANCE_OCTMAP_ARRAY, texture2D otherwise) and binding 8 sky_sampler, and includes restir_core_ddgi_inc.glsl once. The
    sky modes need it for vec3_to_oct_with_border, and they declare the DDGI UBO at set 2 binding 0 too. glsl_builders.py inlines
    a file only at its first #include in a stage and drops every later #include of it, whatever the #if around either, so the
    file must have exactly one #include (a second one under another condition compiles to nothing).
  - _dispatch binds set 2 for the sky modes through _bind_shade, as for FINAL, so TEMPORAL, SPATIAL and FINAL of one run_frame
    read the same sky texture and sampler. run_frame picks the sky twin of TEMPORAL and of whichever SPATIAL mode it would run
    (_sky_twin).
  - Own shader. ShaderRD's validity is all or nothing per version (one variant that fails to compile frees them all), so the six
    sky modes live in a second RestirGiShaderRD per variant (sky_shaders, sky_versions), compiled by _compile into the same
    pipelines table at PASS_SKY_FIRST + i. valid[] covers the 11 old modes only; sky_valid[] covers the six. A sky mode that
    fails leaves the variant running with the flag off, and status sky_reread reads "unavailable". VARIANT_NONE doesn't build
    the sky shader. _ensure builds both at the first frame of a ray variant.
- The sky epoch, decided on the GPU so it has no readback lag.
  - RestirGiResources::sky_epoch is a 32-byte storage buffer (RestirSkyEpoch: float luma, uint epochs, uint jump, uint jumps,
    uint capped, three spare uints; static_assert in restir_events.h), made zeroed on the first frame with the flag on (alloc
    log "sky_epoch"), kept across frames and cuts, and freed with the view's other resources. A trace-size change reallocates
    the state and the epoch starts over.
  - PASS_SKY_EPOCH, one 8x8 group (64 invocations), runs in the frame's ray variant after VALIDATE and before TEMPORAL. It binds
    set 0 only: binding 0 the frame UBO (ubo[0]), 26 the event UBO, 29 the epoch buffer, 30 the same sky texture, 31 the linear
    mipmapped sampler. MODE_SKY_EPOCH undefines RESTIR_RAYS, so the mode declares no set 1 or 2 and _dispatch binds neither.
  - The mean sky luma. F_REAL_SKY clear: luma(min(test_sky, 60000)). A sky texture (RS_SKY_SKY): the mean over the texels of the
    lowest mip, textureQueryLevels - 1 (layer 0 in the array form), of luma(min(texel * sky_energy, 60000)), the per-texel
    transform of rs_sky_radiance. A mip wider than 64 texels is strided to at most 64 rows of at most 64 texels (the radiance
    octmap's lowest mip is much smaller). Invocation i sums row i left to right into a shared slot with its texel count, and a
    fixed tree (halves 32, 16, ..., 1, a barrier after each) adds slot i + half into slot i; invocation 0 divides. The order of
    every addition is fixed, so the mean is the same every run on one device. Border texels count, and the sky's rotation
    doesn't change a mean. A constant colour (RS_SKY_COLOR): luma(min(colour * energy, 60000)). Neither: 0. That mip is not the
    one rs_sky_radiance samples (textureLod at lod 2): it is more heavily filtered and its absolute luma differs, but both scale
    linearly with sky energy, and the rule below only compares ratios.
  - The rule, restir_sky_epoch_step in restir_events.cpp, which invocation 0 of MODE_SKY_EPOCH mirrors line for line (the
    mirror takes the luma, so it has no reduction): the jump word is cleared; a NaN or infinite luma changes nothing else; with
    no epoch yet (epochs 0) or no history this frame (F_HISTORY clear: a cut, the first frame, or the history test option off)
    the luma is stored and epochs grows by 1, without a jump; otherwise the frame jumps when luma < 0.8 * epoch luma or luma >
    1.25 * epoch luma, stores the luma, and epochs and jumps grow by 1, and capped grows by 1 when the jump caps (next bullet).
    The test multiplies, so the ratios 0.8 and 1.25 are inside and an epoch at luma 0 jumps on any light. epochs counts every
    epoch started: the first frame, every cut and every jump, so a flat sky over a run with no cuts reads 1, and in general
    epochs = 1 + cuts while the flag ran + jumps. Doctests "[RestirEvents] Sky epoch ..." and "[RestirEvents] Sky jump counts
    as capped ..." in tests/test_restir_events.h.
  - The anchor is the luma of the last epoch, not a running mean, and moves only at a jump or a cut. A slow drift that stays
    within the band of the anchor never jumps; a steady dawn or dusk ramp crosses it again and again, about once per factor of
    1.25 in sky luma, and each crossing is one jump frame (at jump_m_cap 0, one frame with no temporal history). That is the
    design rule (2.9), which assumed weather-scale drift, not a time-of-day ramp.
  - TEMPORAL's sky mode reads binding 29 (readonly) and treats the jump word as commit 5's jump frame: in rs_event_cap_history,
    after the gates.z cap, m = min(m, jump_m_cap) when the word is set. That function runs under F_EVENTS only, and jump_m_cap
    comes from the event UBO, so the sky jump caps M only with gi/event_reshade on (design 8.1: jump frames are "with
    gi/event_reshade"). With events off the epoch is still followed and counted and nothing is capped. The jump cap is TEMPORAL
    only (commit 5), so the sky jump doesn't reach SPATIAL.
  - capped counts the jumps that lowered the cap: the epoch pass adds 1 at a jump when F_EVENTS is set, the CPU jump word
    (gates.z) is clear (otherwise that frame was capped already) and jump_m_cap (counts.w) < m_temporal_cap (otherwise the min
    changes nothing). restir_sky_jump_caps is the CPU mirror of that test. It counts frames where the cap applied, not pixels
    whose M went down.
  - Not carried over from the CPU jump frame, because the CPU learns of a sky jump only through a readback: VALIDATE "all" at
    jump_m_cap > 0 (it runs before the epoch pass, and it re-shades non-sky samples, which carry no sky Lo), and the SPATIAL hold's
    clear on a jump frame. Status jump_frame stays the CPU's jump.
- One sky per frame. run_frame takes the sky texture once from RestirGiInputs and binds that RID in the epoch pass, TEMPORAL,
  SPATIAL and FINAL. The windowed sky refresh must not land between TEMPORAL and FINAL (design 2.9, memory
  water-sky-refresh-windowed-only); the four passes are recorded back to back in one run_frame call, so a refresh could only land
  between them from inside run_frame.
- Switching is a history cut. A sky sample stored under the flag has Lo 0, and without the flag a history sample with p̂ 0
  still counts its M in the combine (rs_fill_neighbour leaves it on, rs_combine adds its M to total_m), which would divide the
  fresh sample's W by 1 + M and darken every sky-facing pixel for several frames. So resample cuts the history whenever the
  effective flag differs from the last frame's (RestirGiState::sky_reread): the setting toggled, the shading inputs or DDGI UBO
  gone or back, or the sky modes unavailable. The cut goes through the epoch planner's backend test (hardware_changed or the
  flag changed), so cut_counts files it under RESTIR_CUT_BACKEND; sky_reread_cuts counts the ones the flag caused (backend cut
  with the hardware unchanged). The cut also clears F_HISTORY for that frame, so the epoch pass restarts the epoch at the
  current luma, without a jump, when the flag comes back on.
- Cost: one sky sample (textureLod at lod 2, as rs_sky_radiance) per sky technique evaluated in TEMPORAL and SPATIAL and one per
  sky pixel with W > 0 in FINAL; the epoch pass is one 64-invocation group over the lowest mip; 32 bytes of buffer.
- Status.
  - sky_reread: "off" (setting off), "inactive" (setting on, no ray scene, shading inputs or DDGI UBO this frame), "unavailable"
    (setting on, the frame's ray variant failed to build its sky modes), or "on"; the last view to run.
  - sky_epochs, sky_jump_frames_total and sky_jump_capped_total: the epoch buffer's epochs, jumps and capped, summed over the
    views. sky_reread_cuts_total: the history cuts the flag caused, summed over the views. All count over a view state's life,
    not the backend's.
  - The epoch counters are cached, never read in get_status: get_status is const, runs on whatever thread asks, and a
    buffer_get_data there is legal only on the render thread and stalls the GPU queue even there. resample refreshes each
    view's copy right after read_stats on frames with the readback test option on (options.readback), the frames that already
    stall for the probe stats, so the 32-byte read adds no stall. sky_epoch_source is always "cached"; sky_epoch_frame is the
    frame index of the newest refresh, -1 when there was none, and the three totals are 0 until a refresh. A lab that wants
    them turns readback on for at least its last frame.
- Identity with the setting off: sky_reread "off", bit 26 clear, no epoch buffer, no epoch pass and no history cut from the
  flag, and the plain TEMPORAL and SPATIAL modes run from the first shader. The sky shader is still built when a ray variant is
  first used; it isn't dispatched. The first shader's source changed only under #ifdef RS_SKY_REREAD, MODE_SKY_EPOCH and in
  preprocessor guards for the TEMPORAL, SPATIAL, CLEAR, CAPTURE and PACK modes, so their SPIR-V should stay byte-identical to
  eabba5d2a. FINAL and VALIDATE gained run-time tests of bit 26, so their SPIR-V changes and their output must not. SOURCE keeps
  its declarations and order through the set 2 restructure. No core shader changed, so every DDGI, RTGI and direct-shadow
  variant stays byte-identical.
- Measured on a plain sky step (14 Nov 2026, lab scene restir_sky_step, sky energy times 2, RTX 4090). restir-skyraw-4090
  ran the module's output alone (module cap 1 and rtgi target_samples 1, record confidence 1.0): setting off and on agree
  to 7e-5 on the open ground, and in both arms the module takes the new sky within one frame of its epoch, 8 frames after
  the step. Verdict for a plain step: the re-read triggers, does no harm and brings no benefit. The two earlier runs
  (restir-skystep, its second run) did not isolate the module: its share of the output was 4/6 and 1/6. A slow sky ramp is
  not tested yet. All of these ran with TAA on and carried the fault in "TAA jitter in the receivers" below.

## Traced tiles only and M in PACK (gi/trace_all_tiles off, gi/confidence)

Design 2.10 (N7 and "PACK writes M"), 3.2, 3.3, 8.1 rows 12-14; change plan commit 9.
- Flags. F_TILE_SKIP is rf.flags bit 27 (134217728) and F_PACK_M bit 28 (268435456); bit 19 stays reserved for Fold. _upload
  sets them from RestirFrameParams::tile_skip and ::pack_m alone and masks both out of the caller's flags (as bits 22 and 26),
  so a harness capture can't set them.
- The tile texture. R2's CLASSIFY output (RB_SCOPE_RTGI "tiles": R32_UINT, one texel per 8x8 tile of trace pixels, storage and
  sampling usage, written by CLASSIFY as an image) is bound at module set 0 binding 27 as `utexture2D rs_tiles` in TEMPORAL,
  every SPATIAL mode, their sky twins, FINAL and PACK. CLASSIFY keeps its storage usage; nothing in R2 changes. It reaches the
  module through the hook field RtgiFrameInputs::tile_texture (design 8.2's hook table), which rtgi.cpp fills with the same RID
  its CLASSIFY and trace bind. A tile is traced when RTGI_TILE_TRACE (bit 0 of the texel) is set, the test ddgi_trace's
  MODE_RTGI makes.
- rs_tile_traced(pixel) returns true without a read while F_TILE_SKIP is clear. Otherwise it reads the texel at
  pixel / RTGI_TILE, clamped to the texture's last texel. The module's groups are 8x8 trace pixels and RTGI_TILE is 8, so every
  invocation of a group reads the same texel and the skip is uniform per group; the barriers in rs_stats_store and FINAL sit
  outside the branch anyway. restir_gi.cpp static_asserts RESTIR_TILE_SIZE against RtgiPass::TILE_SIZE.
- The dummy. With no texture in RestirGiInputs, RestirGiCore binds a 1x1 R32_UINT texture holding RTGI_TILE_TRACE (freed in
  free()). With the clamp, a read of it means "traced" everywhere, so a pass with bit 27 set and no texture runs as with every tile traced.
  The frozen-frame PACK (test option freeze) is a different case: it keeps the last real frame's bits 27 and 28 and is handed
  this frame's tile texture, so a tile setting changed during a freeze takes effect only when the freeze ends.
- The plan, restir_plan_tiles in restir_events.cpp: gi/trace_all_tiles on gives "all" (bit 27 clear, nothing read); off with a
  texture of ceil(trace size / 8) texels in both axes gives "skip"; off with no texture, a zero trace size or another size gives
  "unavailable", where the module runs every tile as before while R2 traces its own tiles (the old unsupported state).
  run_frame clears tile_skip again when the inputs carry no texture (the harness replay).
- What a group in an untraced tile does:
  - TEMPORAL: no candidate read, no history read, no rays. It stores rs_empty() and info = 0. Its pass-stats row is zero.
  - SPATIAL, every mode: no taps and no rays. It stores rs_empty() and leaves info alone (TEMPORAL wrote 0 this frame). It
    doesn't write sel_image, as for an invalid receiver today.
  - FINAL: vis_state = 0, radiance (0, 0, 0, 1), coverage 0, debug 0 under F_DEBUG_OUT (what an invalid receiver gets), info
    untouched. The group's stats row is still written, all zero (partial[] starts at 0), so it carries no counts from an
    earlier frame into the host's reduction.
  - PACK: NO_CANDIDATE | NOT_TRACED, radiance 0, pdf word 0, tag 0. R2's TEMPORAL then zeroes its history there (no THINNED,
    so no carry) and its coverage is 0, so DDGI, surfels or SSGI show.
  - VALIDATE, CLEAR, SOURCE, CAPTURE and SKY_EPOCH don't read the texture. VALIDATE works on the previous history in place;
    a tile untraced last frame holds empty reservoirs there, which VALIDATE's M 0 early-out skips, so no budget goes to them.
    SOURCE (gi/own_sampler, interim) still traces every pixel; TEMPORAL ignores those candidates in untraced groups.
- Taps from a traced group into an untraced tile are not techniques (design 2.6). The offsets path tests the tap's tile after the
  frame test and before rs_receiver_cur, and files it as REJ_UNTRACED (14; 13 stays free for Fold's REJ_BOUND) in the spatial reject nibble. The compatibility
  selection skips such a tap before scoring, as an out-of-frame tap, so its pick slot goes to a traced tap; it records no
  reason, so a receiver whose taps all fall in untraced tiles reads REJ_NO_COMPAT (12) there, and 14 shows on the offsets path only. The tapped
  reservoir is empty anyway (TEMPORAL and an earlier SPATIAL pass wrote rs_empty() there), so the gate only saves fetches and
  pick slots; the selection depends on geometry and tile flags only. FINAL's 3x3 W median already skips M 0 neighbours.
- Re-trace: a tile traced again starts from empty reservoirs (the history tap reads M 0, REJ_NO_SAMPLE) and vis_state 0, so its
  first FINAL counts from 0 (G-R1(j)). Spatial reuse from traced neighbours fills it.
- No history cut on a change of the setting or the tile state: turning the skip on empties untraced tiles within the frame, and
  turning it off lets the empty tiles fill from the candidates.
- Readback stats with the skip: FINAL rows of untraced groups read 0, so geometry, output_pixels, m_sum and the record's
  sample_count (mean M) and validity cover traced pixels only. The info-based reject counts already skip info 0.
- M in PACK, gi/confidence:
  - frames (default): the pdf word stays 0, today's output. Design 3.3's frames is the old form, coverage from R2's history
    count alone (n̄ / T, T = target_samples), RG-2's O pair's first arm; M takes no part.
  - frames_and_m sets F_PACK_M, whatever gi/trace_all_tiles says (RG-2's O pair runs it with every tile traced). PACK reads the final reservoir set (binding 7, reservoir[final_index], which run_pack now binds)
    and writes float(M) (the low 16 bits of layer 1's w) into the pdf word wherever it packs a candidate (flags 0: a covered
    pixel, or a blocked one on the INFO_VIS_BLOCKED path), in traced tiles only. THINNED and NO_CANDIDATE pixels keep 0.
  - R2 doesn't read the word until commit 10 (module-output bit 256: the EMA of min(M / n_target, 1) into moments.w, route
    coverage with T_r). With commit 9 alone frames_and_m changes candidate_second's z word and no image.
  - restir_pack_word in restir_events.cpp mirrors PACK main line for line. Doctests "[RestirEvents] Tile plan ..." and
    "[RestirEvents] Pack word ..." in tests/test_restir_events.h.
- Status: tiles ("all", "skip" or "unavailable"; the last view to run), tile_skip_frames_total (frames run with the skip, over
  the backend's life, every view), confidence ("frames" or "frames_and_m").
- Identity with the defaults (gi/trace_all_tiles on, gi/confidence frames): bits 27 and 28 clear, rs_tile_traced returns true
  without a read, every pass takes its old path, and PACK's pdf argument is 0.0 as before. The new bindings 27 (and PACK's 7)
  and the run-time tests change the SPIR-V of TEMPORAL, the four SPATIAL modes, their five sky twins, FINAL and PACK; their
  output must not change. CLEAR, SOURCE, CAPTURE, VALIDATE and SKY_EPOCH see only new #defines, so their SPIR-V should stay
  byte-identical to 45bee90de. No core shader changed.

## R2 side: module output, route coverage, tile memory

Design 2.8 (R2's two restart branches), 2.10, 3.2, 3.3, 8.2 rows 5-8; change plan commit 10. The core part (rtgi.h,
rtgi.cpp, rtgi.glsl, rtgi_inc.glsl, gi_module_hooks.h, the rtgi/ddgi_wrong_* settings in rendering_server.cpp) never names
the module; the module part is restir_route_output and its use in restir_gi.cpp.
- Hook fields. RtgiResampleOutput gains mode (RTGI_OUTPUT_PLAIN, the default, or RTGI_OUTPUT_MODULE) and n_target (0 means
  none). history_cap was already T_r (gi/denoise_history_cap). Design 8.2's hook table names both.
- When the module says "module output". restir_route_output(denoise, events_on, pack_m, n_target) in restir_events.cpp:
  module output when gi/denoise is r2 and either events are on (gi/event_reshade with gi/replay_seed) or gi/confidence is
  frames_and_m; n_target (gi/n_target, 1..64 from the setting; the function's own 1..255 clamp is never reached) only with frames_and_m. So:
  - defaults (events off, frames): plain. Bit 256 stays clear, and R2 with the module on does what it did at 697371ea0,
    restarts included. This is the design's "every new setting defaults to the P4 head's behaviour".
  - events on, frames: bit 256 set, n_target 0. TEMPORAL skips R2's restarts (design 2.8: the module already restarted the
    reservoirs, "otherwise events mode restarts twice"); moments.w carries the EMA of 0 (the pdf word is 0); COMPOSE keeps the
    old form with T = target_samples. RG-1's event arms run here.
  - frames_and_m (events on or off): bit 256 set and n_target handed over: route form (3.3). RG-2 runs here.
    With events off the skip of R2's restarts still holds, for design 2.8's other reason: a sphere restart sets R2's count
    to 0, and in route form that count feeds coverage, which would dip to 1/T_r on every event frame. Nothing local
    replaces the restart in this arm: the module's reaction to a lamp event is the screen-wide m_light hold, which lowers
    coverage everywhere through the M term. RG-2 measures this arm; it is a ruling from the design, not a measured result.
  - The frozen-frame test option reuses the last real frame's route.
- Core sets RTGI_FRAME_FLAG_MODULE_OUTPUT (256, free between 128 and REPLAY 512) only when the backend's output is used
  (denoise valid) and mode is RTGI_OUTPUT_MODULE. It is set after the backend runs, so CLASSIFY and the trace never see it; the
  frame UBO is uploaded again before TEMPORAL (as for every denoise frame).
- TEMPORAL under bit 256:
  - the early return is unchanged: zero history gives moments.w 0;
  - the gather carries tap_moments.w with the bilinear weights, as .x and .y;
  - the global soft cut and the restart-sphere cap are both skipped. FRAME_FLAG_RESTART_SPHERES and the sphere list are still
    set and uploaded (R5 reads them), so RTGI_COUNTER_RESTARTED reads 0 on an event frame with the bit set;
  - the carry store writes the gathered .w; the normal store writes mix(w_previous, min(M / n_target, 1), blend), with the same
    blend as the other moments and M = uintBitsToFloat(candidate_second.z), the word PACK writes under frames_and_m. With
    n_target 0, or a word that is not above 0 (NaN included), the new sample is 0.
  - The CPU soft-cut bookkeeping (frames_since_cut held at soft_cut_count, which feeds the published record's sample_count)
    still runs; design 3.3 keeps the record "as today".
- Route form (bit 256 and n_target above 0). Core writes T_r (the route history cap, already in settings.history_cap) into
  FrameUBO.target_samples and n_target into the new word; plain R2 and the old route keep target_samples. The published
  record still uses rtgi/target_samples. COMPOSE then sets confidence = min(clamp(n̄ / T_r), clamp(μ̄ / N)) in place of
  clamp(n̄ / T), with μ̄ the bilateral mean of moments.w over the same taps and weights as n̄ and N = 1. Validity and support
  multiply as before.
- Marked tiles in route form. COMPOSE reads R2's tile texture (new COMPOSE binding 10) at the tile of the trace pixel nearest
  the slot pixel, only while R5 or R6 is on. A marked tile (RTGI_TILE_REASON_DDGI_WRONG, tile bit 64) with n̄ ≥ 2 and a
  relative deviation sqrt(m2 − m1²) / max(m1, L_floor) below rtgi/ddgi_wrong_noise takes T_r = 1 and
  N = rtgi/ddgi_wrong_target / n_target (clamped to 1e-3..1, so a target above n_target cannot hold coverage below 1). m1 and m2 are the bilateral means of moments.x and .y over the
  same taps; design 3.3 says "per-pixel", and these are the slot pixel's own taps.
- Tile memory (design 3.2). Two R16_UINT textures, "tile_memory_0" and "_1", one texel per tile, ping-pong on R2's frame index
  (read previous, write current), created with R2's other textures and cleared at creation. Low 8 bits: frames left;
  bit 8 (256): set by R5; bit 9 (512): set by R6.
  - Bits in FrameUBO.ddgi_wrong_bits: ACTIVE 4 (R5 or R6 on: the memory is written), READ 8 (no cut this frame and the last
    frame that ran wrote memory), EVENTS 1 (R5 on and this frame's sphere selection did not overflow), MEMORY 2 (R6 on and
    no cut). A refused or skipped frame leaves READ clear on the next one, so a stale texture is never read. A cut, resize
    or trace-scale change therefore reads nothing (the textures are recreated on resize). CUT_MODULE is decided after
    CLASSIFY, so on that frame the memory is still read.
  - Reprojection: one trace pixel per tile reads, the centre (local 4, 4), or on a cut-off edge tile the in-range pixel
    nearest it. If it has geometry it takes its view position to the previous view and clip, rounds to the nearest previous
    trace pixel, and reads the previous memory at that pixel's tile. It reads nothing when the point is behind the previous
    camera or off the trace, the previous geo_position there has no geometry, or the previous position lies farther than
    TEMPORAL's depth tolerance from the plane through the reprojected point (the current normal turned into the previous
    view; CLASSIFY binds no previous normal). A plane test, as in TEMPORAL: a 3-D distance would also count the rounding
    offset inside the surface, which at grazing angles exceeds the tolerance once the camera moves. R6's per-pixel history
    reads use the same test.
  - Known limits, for RG-2 to record: a tile whose reading pixel is sky reads nothing, so a mark on a silhouette tile lasts
    one frame; switching gi/confidence or events on a running frame is not a cut, so moments.w starts at 0 and route
    coverage ramps over about the history cap; with gi/confidence_ramp on, T_r is the ramped cap, not
    gi/denoise_history_cap (the defaults make them equal); the noise gate's m1 and m2 are bilateral means, so the deviation
    reads high across texture and shadow edges and the gate shuts there (the slow side).
  - Per frame: left = stored − 1 (0 stays 0); the marks clear when left reaches 0. R5 sets left = rtgi/ddgi_wrong_frames and
    bit 8 when any trace pixel of the tile lies inside one of the frame's view-space restart spheres. R6 sets left and bit 9
    on a tile traced by today's reasons or by memory when its test passes. A tile with left above 0 gets tile reason 64
    and RTGI_TILE_TRACE. A marked tile is traced on the marking frame and ddgi_wrong_frames − 1 frames after it. Tiles with
    no geometry write 0.
  - R5 overflow: when R2's selection dropped spheres (more than 16), EVENTS stays clear that frame and r5_overflow counts it.
    The list is uploaded for R5 even when R2's own sphere restart is off or the frame is a soft cut; TEMPORAL reads it only
    under FRAME_FLAG_RESTART_SPHERES, as before.
  - R6's test (rtgi_tile_disagrees): each trace pixel with geometry reprojects as above and reads last frame's history; if
    its count is above 0 it adds (luma, luma², 1) to a 64-entry shared array, which thread 0 sums. m_R is the mean, var_R the
    sample variance (÷ (n − 1)), n_eff = n / (1 + ρ (n − 1)) and SE_R = sqrt(var_R / n_eff). m_D is the mean luma of the classify
    taps that ran and have DDGI alpha above 0 (shared_ddgi is now vec4[4]: rgb and alpha). Mark when n ≥ 2, at least one tap,
    |m_R − m_D| / max(m_D, L_floor) > τ and SE_R < τ m_R / 3. Both means are before DDGI's energy factor, which COMPOSE and
    gi.glsl apply alike.
- FrameUBO grows by two 16-byte rows after restart_spheres: (ddgi_wrong_frames, ddgi_wrong_bits, n_target, ddgi_wrong_rho) at
  624 and (ddgi_wrong_tau, _target, _noise, _floor) at 640; 656 bytes, three static_asserts. The ddgi_wrong_* words stay
  zero unless R5 or R6 is on, and n_target unless the route form runs.
- Settings (internal, next to the other rtgi/* ones; none is in ProjectSettings.xml, as for every internal rtgi setting):

| Setting | Range | Default |
|---|---|---|
| rtgi/ddgi_wrong_events | bool (R5) | false |
| rtgi/ddgi_wrong_memory | bool (R6) | false |
| rtgi/ddgi_wrong_frames | int 1..255 | 16 |
| rtgi/ddgi_wrong_tau | float 0..16 | 0.5 |
| rtgi/ddgi_wrong_target | float 0..255 (N_w) | 2.0 |
| rtgi/ddgi_wrong_noise | float 0..16 | 0.25 |
| rtgi/ddgi_wrong_floor | float (L_floor) | 0.05 |
| rtgi/ddgi_wrong_rho | float 0..1 (ρ) | 1.0 |

  L_floor and ρ have no setting in design 8.2; they are settings here so RG-2 can set them. ρ 1 makes n_eff 1, the side that
  marks least, until RG-2 registers ρ. L_floor 0.05 is a placeholder in luma before DDGI energy.
- Status. R2: ddgi_wrong {bits, r5_frames, r5_overflow}, route {module_output_frames, route_frames, n_target}, and with
  rtgi/gpu_counters the counters tiles_wrong_events and tiles_wrong_memory (tiles carrying bit 8 or 9 this frame). Module:
  module_output, route_n_target, module_output_frames_total. no_canonical is already in stats (commit 4). There is no engine
  spikes key: a spike is a pixel above 10 times its 3x3 median (G-R3(b)), plain R2 has no module stats row, and the
  registration has the reader count spikes with one method for every arm, so the lab fills that column from the reader.
- Doctests: "[RestirEvents] Route output ..." in tests/test_restir_events.h cover restir_route_output.
- Identity. With the module off, or on with the defaults, bit 256 is clear; with R5 and R6 off the new UBO words are 0. Then:
  - CLASSIFY reads and writes no memory, takes the same tile reasons (alpha from shared_ddgi[i].a, initial alpha 1.0) and
    writes the same geo and tile texels;
  - TEMPORAL takes both restart branches as before and stores moments.w 0 in both stores;
  - COMPOSE keeps clamp(n̄ / T) and reads no tile;
  - VARIANCE, ATROUS and DEBUG only see the longer UBO block.
  The SPIR-V of all six rtgi.glsl modes changes (the UBO block; CLASSIFY, TEMPORAL and COMPOSE also their code and bindings;
  with rtgi/gpu_counters the counter array is 11 words); their outputs must not. ddgi_trace's MODE_RTGI and every module
  shader see only new #defines in rtgi_inc.glsl, so their SPIR-V should stay byte-identical. R2 allocates the two tile-memory
  textures (2 x tiles x 2 B) whatever the settings, because CLASSIFY declares them.

## Fold, the stored-age bound (gi/fold_k, default 0)

FOLD-ENGINE-NOTE sections 1 to 3 (the rule, storage, clock, K and the pre-frame), STAGGER-NOTE section 3 (carry, renewal by
fold, start from π), B1-NOTE's refusal and proof; change plan row 11 and section 5 items 1 to 9, 11 and 12. Item 10
(last_lit[view] and the OS window's item 1) is GI's, in ray_scene.cpp, and is not in this commit: nothing here stamps or
reads a light.
- Scope and K_eff. gi/fold_k is read through restir_fold_k (restir_fold.cpp): 0 or less is off; otherwise the value is
  clamped to 4..64 and rounded down to a power of two (5 reads 4, 63 reads 32, 1000 reads 64). K_eff (restir_fold_k_eff) is
  that K in B1, B2 and B2k with spatial feedback off, and 0 in U1 or with feedback on, whatever made it on (the mode's
  default or the spatial_feedback test option). _upload takes its three Fold words (flags, clock, K) from one pure function,
  restir_fold_words (restir_fold.cpp), which rounds the K word again (restir_fold_k) and applies the same gate from
  RestirFrameParams::mode (U1) and spatial_feedback. In the engine those are the values resample gated with, so the words
  don't change; a harness dict with K 7 runs at 4, and one with mode U1 or spatial_feedback true (the RestirFrameParams
  default, so also a dict without the key) runs with Fold off, never at a K or in a mode the fold can't take.
- Storage. s is reservoir flags bits 8 to 13 (RF_S_SHIFT 8, RF_S_MASK 0x3F) and mv bit 14 (RF_MV), in the high half of layer
  1's y; bit 15 stays free; RF_FOLD_BITS (0xFF00) names the byte. No size change: 3 RGBA32UI layers, 48 B per pixel per set.
  A fresh candidate (rs_from_candidate) and rs_empty carry 0 there, and the Fold write after rs_finish is the only writer of
  those bits in TEMPORAL. VALIDATE re-stores flags << 16 whole (budget, share, replay and event paths) and clears them only
  on a dead sample, which also has M 0 and reads as a start. SPATIAL copies a neighbour's flags into set 2 (or into the old
  history set on a second pass), and with feedback off neither becomes history, so the history set holds only Fold's bits.
- TEMPORAL order (rs_temporal_pixel). hp moves out of the F_HISTORY block (still ivec2(-1) when it isn't computed); h_s and
  h_mv are read from h where it is loaded, before rule 1 can clear c[1]. had is c[1].present after the fill (or cleared),
  so a history the fill rejected (the unusable return, a shift limit, B2's REJ_VIS, B2k's kept failures) is present and a
  carry unless rule 1 clears it; the reason code never classifies.
- Rule 1 runs right after rs_fill_neighbour, under K_eff > 0 only: it refuses when h_s + 1 ≥ K, or ≥ K − 1 on a pre-frame
  (rs_fold_refuses). A refusal sets c[1] = rs_cand_none() (so neither its M nor a kept M enters the total), sets cleared,
  sets REJ_BOUND (13) only when the reason is still REJ_NONE, and leaves INFO_HAD_HISTORY as the load set it.
- The write, after rs_finish and only under K_eff > 0: result.flags = (flags & ~0xFF00) | rs_fold_bits(...), never an OR,
  since rs_combine copied the selected technique's bits.
  - carry (had, not cleared): s = h_s + 1, mv = h_mv or hp != px;
  - renewal (cleared): u = (v + φ(px)) mod K when h_mv is 0 and hp == px, else ψ_r(px, v) mod K; s = u for u ≤ K/2, else
    u − K/2; mv = 0;
  - start (no history: REJ_CUT, REJ_FRAME, a similarity reject, REJ_NO_SAMPLE, REJ_EPOCH, the biased REJ_DIRTY): s =
    draw(ψ_s(px, v)), mv = 1;
  - on a pre-frame every birth (renewal or start) is capped at K − 2. A renewal is at most K/2 ≤ K − 2 already, so the cap
    binds on starts only.
  The write runs for every valid traced receiver, also when the stored reservoir is empty (a refusal on a pixel with no
  fresh candidate, c[0].on false: any pixel on a revalidation frame, or one that ray thinning skipped on any frame; or a
  pixel with neither candidate nor history): its M is 0, so the next frame reads REJ_NO_SAMPLE, a start, and the bits there
  are never read.
- The draw (KB9): x = ψ_s mod (3K²/4); s = isqrt(x) if x < K²/4, else K/2 + (x − K²/4) / K. Only x below K²/4 reaches
  rs_fold_isqrt (at most 1023, at K 64). It is uint(sqrt(float(x))) corrected once each way: a float seed is at most one
  off there, and the two corrections cover it.
- Hashes, never rs_rand. φ(px) = rtgi_pcg(px.x + rtgi_pcg(px.y + C1)); ψ_r = rtgi_rng_state(px, v, 0, C2) and ψ_s =
  rtgi_rng_state(px, v, 1, C2). C1 = 0x68E31DA4 and C2 = 0x2C1B3C6D (RS_FOLD_C1, RS_FOLD_C2 in restir_gi.glsl).
  rtgi_rng_state hashes dimension + seed first, so Fold's two streams are C2 and C2 + 1. The other callers pass runtime
  seeds: ddgi_trace's MODE_RTGI and the module's SOURCE take rtgi_rand2(..., RTGI_RNG_DIRECTION, seed) with the
  rtgi/seed setting (default 1; sums 1 to 8 with the doubled dimension), and rs_rand and rs_rand_frame take dimension 64 +
  d with seed rtgi_pcg(seed * 0x9E3779B1 + 0x7F4A7C15), seed rtgi/seed + seed_offset or 0x1234567 under F_DETERMINISTIC
  (0xF5F17964 and 0x56F88133 at the defaults). C2 is about 7.2e8 or more from every one of those sums, and C1 is far from
  C2. The doctest "[RestirFold] C2 sits at least 5 ..." checks the defaults; a non-default rtgi/seed is the run's to list
  in run.json. φ's classes are not balanced (FOLD-ENGINE-NOTE section 3); the R2 side measures them on the trace grid first.
- The clock v is RestirGiState::frames, read before resample's frames++ (params.fold_clock), in shade_pad1; K_eff is in
  shade_pad2. The GLSL names stay shade_pad1 and shade_pad2 (RS_FOLD_CLOCK and RS_FOLD_K alias them inside MODE_TEMPORAL)
  so no other mode's block changes; the C++ struct calls them fold_clock and fold_k_eff. The freeze return comes before
  frames++, and a refused begin_frame never reaches resample, so neither advances v. A state rebuild (begin_frame's
  reallocation, release, R2's free_data and RtgiPass::release, through release) restarts frames at 0 with a
  RESTIR_CUT_FIRST frame. K divides 2^32, so the uint32 wrap is harmless.
- The pre-frame flag is rf.flags bit 19 (F_PRE_FRAME, RESTIR_F_PRE_FRAME 524288; restir_fold.h names it
  RESTIR_FOLD_F_PRE_FRAME and restir_gi_core.cpp asserts the two agree). The revalidation schedule is written once,
  restir_reval_frame (restir_fold.cpp): frame_index % period == period − 1, false for a period below 2; resample's
  revalidation test calls it. restir_fold_pre_frame: revalidate is budget, the ray scene is ready, and restir_reval_frame
  holds for frame_index + 1 (period 2..16, the revalidation period). That equals frame_index % period == period − 2 on
  every frame, the uint32 wrap included (at frame_index 0xFFFFFFFF both are false for every period 2 to 16). It does not
  read use_history, so a cut on a pre-frame still caps its starts. resample sets it only with K_eff > 0, and
  restir_fold_words masks bit 19 out of the caller's flags and sets it from RestirFrameParams::fold_pre_frame only with its
  gated K above 0, as bits 22 and 26 to 28. The schedule runs on the global frame index (FOLD-ENGINE-NOTE section 3's
  skipped-pre-frame cases hold).
- Cuts. restir_fold_cut compares this frame's RestirFoldFrame (K, K_eff, mode, feedback) with the last frame that ran: a
  change of K_eff is a cut; a change of mode or feedback is a cut while either frame has K above 0. With gi/fold_k 0 on
  both frames nothing here cuts, so a mode switch at the default behaves as before. The planner files it as
  RESTIR_CUT_FOLD (8, RESTIR_CUT_MAX now 9) after the request and backend tests; the cut reaches R2 as history_cut
  (CUT_MODULE), as every module cut does. A cut makes every valid pixel a REJ_CUT start (F_HISTORY clear), so the bound
  holds from that frame.
- Stats (pass-stats row 0, TEMPORAL, read with the readback option). PS_STRIDE stays 32, PS_FOLD_REFUSED 22,
  PS_FOLD_CARRIED 23, PS_FOLD_RENEWED 24, PS_FOLD_STARTED 25 and PS_FOLD_REFUSED_EMPTY 26; slots 27 to 31 stay free.
  PS_FOLD_REFUSED_EMPTY counts, inside the same refusal branch, each rule-1 refusal whose fresh candidate is absent
  (c[0].on false, the test TEMPORAL already uses for it); it is a part of fold_refused, not a class of its own. Refusals
  are counted at rule 1 by event, so one whose reason the fill had set is counted too; REJ_BOUND in temporal_reject counts
  only those that kept REJ_NONE. Every renewal is a rule-1 refusal under this rule, so fold_renewed equals fold_refused;
  the reader's disjoint classes are refused = fold_refused, renewed = fold_renewed − fold_refused (0), started =
  fold_started and carried = fold_carried, and their sum is the frame's valid traced receivers. Status: fold_k,
  fold_k_eff, fold_clock and fold_pre_frame (the last view to run), and stats fold_refused, fold_carried, fold_renewed,
  fold_started and fold_refused_empty.
- Harness. params_to_dict gains clock, k_eff and pre_frame, and params_from_dict reads them. A capture made before Fold has
  none, so it replays at K_eff 0; a capture made with Fold on replays the frame's rule from its own history layers.
  run_case's stats dict gains the five counts.
- How it composes.
  - B2k: a kept failure (REJ_VIS or REJ_DIRTY through the fill) is present, so it carries and rule 1 tests it; a refusal
    drops its kept M with the rest of c[1]. Under B2k a dirty history already reaches the fill instead of taking REJ_DIRTY
    before it (F_KEEP_M's existing behaviour, unchanged here), and Fold relies on that: under B2k a dirty history is a
    carry where B1 and B2 make it a start. The bound rests on presence, so it holds.
  - Untraced tiles (F_TILE_SKIP): TEMPORAL stores rs_empty() there (flags 0, so s 0 and mv 0, M 0) and runs no Fold code.
    The next traced frame reads M 0, REJ_NO_SAMPLE, a start. An invalid receiver is the same.
  - Sky re-read: TEMPORAL_SKY is the same rs_temporal_pixel with RS_SKY_REREAD, so it has the same rule; rs_store's sky
    branch zeroes only Lo.
  - Events, replay and revalidation: VALIDATE runs on the history set before TEMPORAL and keeps flags (s and mv) on every
    path that keeps the sample; event caps lower M before the fill and don't change the class.
  - Revalidation budget: the pre-frame refuses at K − 1 and caps births at K − 2, so nothing reaches K on the revalidation
    frame. No pixel has a fresh candidate there, so a pixel refused there would store an empty reservoir. That is one such
    case, not the only one: a pixel without a fresh candidate on any frame (ray thinning) is the other (known limits).
- Doctests, tests/test_restir_fold.h ("[RestirFold] ..."): K rounding and clamping, the K_eff gate, the pre-frame predicate
  against restir_reval_frame and the draft's form (the wrap at 0xFFFFFFFF included), the upload words (bit 19 from the
  caller always masked, set only with K above 0 and a pre-frame; clock and K words 0 at K 0; K 7 reads 4; U1 and feedback
  force 0), the harness dict round trip (clock, k_eff and pre_frame survive; a dict without them reads 0, 0, false), the
  cut on change, the planner's order (Fold after the request and backend cuts, before the origin and scene cuts). The test
  also holds a C++ mirror of the shader's integer rule (isqrt, the draw, rule 1, the write, φ and ψ from rtgi_pcg) with
  known answers: KB9 at every allowed K, the isqrt from a seed one off either way, the bound over every h_s, the bit
  layout, the static relock within 3K frames, and hash values worked out separately. The mirror is not the shader: it
  shows the arithmetic, and only a GPU readback of the shader's routine (check 1) shows the shader.
- Allocations: rf.flags bit 19; reservoir flags bits 8 to 14 (bit 15 free); reject code 13; RestirFrame shade_pad1 (clock)
  and shade_pad2 (K_eff), no growth (400 bytes); pass-stats slots 22 to 26; cut reason 8; seeds C1 and C2; no new binding,
  texture or buffer.
- Identity at gi/fold_k 0 (the default): K_eff is 0, so the UBO words are 0 (_upload writes the clock only with K_eff > 0),
  bit 19 is clear, rule 1 and the write don't run, and no Fold cut happens. Bits 8 to 15 are then whatever rs_combine
  copied, as before (0 everywhere, since nothing else writes them). TEMPORAL's float work is unchanged: the hoisted hp
  keeps its values, and h_s, h_mv and cleared are integer and boolean locals. The new code is inside MODE_TEMPORAL, so the
  SPIR-V of TEMPORAL and TEMPORAL_SKY changes (every ray variant) and their output must not; every other module mode sees
  only new #defines and should stay byte-identical to 22c0ee2ac. No core shader changed. Status gains keys and cut_counts
  one entry; settings snapshot gains gi/fold_k.
- Known limits, for the R2 side's checks: φ is a hash, not a balanced tile; only K 8 and 32 were judged; a stored s needs a
  debug readout of the history set (check 3), not FINAL's set; starts by situation (check 10) and families (check 12) need
  commit 12's debug tags; a view that skips the pre-frame can refuse up to the K − 1 starts on its revalidation frame
  (quality, not soundness); a Fold change while the freeze test option holds takes effect when the freeze ends.
  - Ray thinning: with the RTGI ray_fraction f below 1, about (1 − f)/K of pixels per frame are refused with no fresh
    candidate (a pixel ray thinning skipped, RTGI_FLAG_NO_CANDIDATE from ddgi_trace), store an empty reservoir and read
    coverage 0 (the fallback route) until a candidate arrives. The pre-frame does not cover it; the bound needs the
    refusal, so it stays. fold_refused_empty counts it, and reader check 1 at commit 12 must include a thinned run.
  - C2's spacing from the other rtgi_rng_state streams is proven only for the default rtgi/seed, so a run with another
    seed records it.
  - fold_renewed always equals fold_refused in this commit: every renewal is a rule-1 refusal.
  - Debug view 60 scales its heat by 11, so REJ_BOUND (13) shows saturated.
  - The doctests named "Mirror:" test a C++ copy of the shader's rule; only the GPU readback at commit 12 tests the shader.
    The hash known answers were recomputed independently from the GLSL text and agree.

## TAA jitter in the receivers (`modules/gi_restir/restir_jitter.cpp`)

- What was wrong. R2 writes the geometry position texture (geo_position, view space, one anchor per trace pixel) from the
  jittered depth buffer with the unjittered inverse projection (rtgi.cpp _fill_frame_ubo: inverse(C * cam_projection), C the
  depth correction with the y flip). The depth was drawn with get_cam_projection() = C_j * cam_projection, where C_j is C
  with the TAA jitter added to columns[3][0] and [3][1], so the drawn NDC is the plain NDC plus the jitter. The stored
  position is therefore moved sideways by the jitter; on a surface seen at a slant that puts it above or below the true
  surface. R2's own trace does not read that texture for its origin: ddgi_trace MODE_RTGI rebuilds the position with
  drawn_projection.inverse(). The module did read it: rs_receiver_cur and rs_receiver_prev built r.pos, r.origin and the
  four origin-offset taps from it, and every visibility ray (rs_visible) starts at r.origin, so on some jitter phases the
  origins sat under the surface and the rays hit it.
- Measurement (14 Nov 2026, restir-skydiag-4090 and restir-skyfloor-4090, engine run copies at 697371ea0 and, for one
  leg, eabba5d2a, which read the same). TAA on: the module's raw output went dark on 5 of every 16 frames (2 of 8 under
  FSR2, whose jitter cycle is 8); TEMPORAL rejected about 98 % of pixels with REJ_VIS (11) on 11 of 16 frames; M stayed
  near 1 per pixel. TAA off: no dark frame, M reached 31.6 per pixel. With R2's denoiser after the module the frame read
  21 % under its TAA-off level. rtgi/origin_floor 0.004 with TAA on removed every dark frame and brought M to 31.6, which
  places the fault in the ray origins.
- Every module figure measured with TAA on before this fix carried the fault: the raw output was dark on 5 of 16 frames
  and temporal reuse was rejected on 11 of 16. That includes the sky-step runs. Those figures are not comparable with runs
  after this fix.
- The fix. R2 hands two more projections to backends (gi_module_hooks.h RtgiFrameInputs: drawn_projection and
  previous_drawn_projection; RtgiBuffers keeps previous_drawn_projection beside previous_projection, with the same
  have_previous fallback). Nothing in R2's shaders, UBOs or behaviour changes. The module turns each pair (P, D) into a
  view-space shift with restir_jitter_shift (restir_jitter.cpp, pure, in double): true.xy = view.xy + view.z * a + b and
  true.z = view.z, where view is rebuilt with inverse(C * P) and true with inverse(D) from the same (uv, depth).
  - Derivation: D = T * M with M = C * P and T the identity plus (jx, jy) in column 3, rows 0 and 1. A point t = view + d
    with d.z = 0 lands, under D, where view lands under M exactly when A d = -j w, where A is M's upper-left 2x2 (clip xy
    from view xy) and w = M[2][3] z + M[3][3] is the clip w. So with q = A^-1 j: a = -q * M[2][3] and b = -q * M[3][3].
    Indices here are Godot's columns[c][r]: M[2][3] is column 2, row 3. Perspective: a = (jx / M[0][0], jy / M[1][1]),
    b = 0 (M[1][1] carries the flip). Orthogonal: a = 0, b = -q.
  - It recovers j from the column whose row-3 entry is largest (column 2 for perspective, 3 for orthogonal) and then
    checks, to 1e-6 of each row's scale, that D's rows 0 and 1 are M's plus j times M's row 3, that rows 2 and 3 match, that
    M's z and w rows have no x or y term, and that A is invertible. If any check fails (a y flip, another camera, a depth
    change) it returns zero and sets unmatched; it never returns a partial shift.
  - D equal to M entry by entry (an exact comparison) returns zero at once, so a frame without jitter uploads zeros.
- Plumbing. RestirFrameParams gains jitter_shift and previous_jitter_shift (a.x, a.y, b.x, b.y as floats), from this
  frame's pair and from the previous pair. RestirFrameUBO grows from 400 to 432 bytes: two vec4 at the end of RestirFrame
  (jitter_shift, prev_jitter_shift, offset 400 and 416, std140-aligned). params_to_dict and params_from_dict carry both as
  PackedFloat32Array; a capture without them replays with zeros, as before. Status: jitter_shift (four numbers),
  jitter_ndc (the recovered jitter), jitter_unmatched (this or the previous pair) and jitter_unmatched_frames_total.
- Freeze. The freeze return (test option freeze) builds no RestirFrameParams: run_pack uploads nothing and reuses ubo[3]
  from the last run_frame, which carries that frame's shifts, and PACK reads only geo_position's w, which the shift never
  touches. So there is nothing for the freeze to hold; its status keeps the last real frame's shift. The previous pair always comes from R2, which runs every frame, so after a
  freeze the previous shift matches the previous position texture R2 hands over.
- Shader. One helper, rs_true_view(view, shift) = vec3(view.xy + view.z * shift.xy + shift.zw, view.z). Receiver gains
  true_view; r.view stays the stored (unshifted) position.
  - True position (geometry): rs_receiver_cur (current shift) and rs_receiver_prev (previous shift) build r.true_view,
    r.pos from it, and r.origin through rs_origin_offset_vector with the true centre and the four neighbour taps each
    shifted with its own z (w kept). Everything downstream reads r.pos or r.origin and so is true: the plane test in
    rs_similar, the jacobians (rs_shift_jacobian, rs_jacobian_raw), rs_phat and rs_dir_to, every rs_visible ray (TEMPORAL,
    SPATIAL, FINAL, VALIDATE), the fresh-Lo and replay rays (FINAL, VALIDATE), SOURCE's own-sampler ray, the event and hold
    spheres (rs_event_caps, rs_hold_caps), the dirty-box segments (rs_dirty_hit), and the replay distance tests in
    VALIDATE. The compat score (PASS_SPATIAL_COMPAT, rs_compat_score) measures receiver distances, so it reads true_view.
  - Unshifted: TEMPORAL's reprojection rf.view_to_prev_clip * r.view. That matrix is built from the unjittered projections
    and maps a pixel centre to a pixel centre, as R2's rtgi_previous_trace_pixel does with the same texture. Also unshifted:
    r.depth (z is unchanged anyway), rtgi_anchor_pixel (pixel grid), PACK (only w), CAPTURE (copies the raw textures; a
    replay applies the shifts from its params), CLEAR, SKY_EPOCH and the debug views (no positions). The candidate decode
    takes R2's hit point as stored; R2 traced it from the true origin, so it is not rebuilt from the receiver.
- Identity. With both shifts zero every new float operation is x + z * 0 + 0, which returns x for every finite x (a -0 x
  can come back +0, which compares equal). No other float work moved, there is no setting, and the fix is always on. R2
  writes a zero position with w 0 for a pixel without geometry; under an orthogonal camera the shift turns that into the
  finite point (b.x, b.y, 0), which no pass reads, since every reader tests valid first. The UBO grew, so every module
  mode's SPIR-V changes; no R2 shader changes.
- Doctests, tests/test_restir_jitter.h ("[RestirJitter] ..."): perspective (60 degrees, 16:9, 0.05 to 2000), an off-axis
  frustum and two orthogonal cameras, each with jitter (0.3 / 640, -0.45 / 360) * 2 and its sign flips, D built as
  get_cam_projection builds it; for five uv and five depths the shift maps the inverse(C * P) rebuild onto the inverse(D)
  rebuild to 1e-9 of the point's length, and the zero shift does not. Zero jitter gives exact zeros; a y-flipped D, a
  depth change, a zoom and another camera give zeros and unmatched; the orthogonal shift has no z term. Sign: a positive x
  jitter moves the point toward -x in view space and a positive y jitter (NDC y down after the flip) toward +y. The
  harness round trip keeps both shifts and an old capture reads zeros. The map tests apply a C++ copy of rs_true_view, so
  they test the function and the formula, not the shader. The shader's check is on the card: no dark frame with TAA on at
  origin_floor 0, and status jitter_shift near (jitter_ndc.x / P[0][0], -jitter_ndc.y / P[1][1], 0, 0) for a perspective
  camera (the y term changes sign through the flip). The tolerances are set for a double real_t; the single-precision
  ones are untested.
- Known limits.
  - The tiny origin offset (origin_scale ULPs plus the raster footprint) still assumes the position is exact; this fix makes
    the position right but does not widen the offset. origin_floor stays 0 by default.
  - The anchor is the trace pixel's half-pixel point (trace_pixel * scale + scale / 2); the shift corrects the jitter only,
    not the sampled depth's own footprint inside a trace pixel.
  - Orthogonal and off-axis cameras are tested only in the doctests; no GPU run used them.
  - rs_origin_offset_vector turns a tap normal into world space with the current camera's rotation also for previous
    receivers (as before this fix), so with a turning camera a previous receiver's offset points slightly off its normal.
    This fix does not change it.
  - A pair that does not match (jitter_unmatched true) gets no shift and keeps the old fault for that frame.
  - restir_jitter_shift rebuilds C with set_depth_correction(true), a second copy of R2's correction. If R2's ever
    changes, every frame reads unmatched: zero shift, the old fault, and jitter_unmatched_frames_total counting.
  - R2's own passes are unchanged and still read the stored (unshifted) position: rtgi_previous_trace_pixel, R2's
    temporal plane test and its DDGI lookup. With denoise r2 after the module, some TAA-on difference from the TAA-off
    level may remain for that reason; it is R2's to fix.

## Run counters

Change plan commit 12, sub-commit 12a (C12-DESIGN section 2.1): CPU only. No shader file, no UBO, no pass and no setting
changes; nothing here alters what a frame computes. Everything below is read by the lab through the probe (frame records,
fold rows, seeds) or through status.

- Frame records. RestirFrameRecord gains RestirFoldRecord fold (restir_test.h), every field int64 with -1 for not
  available: tag, serial, clock, k, k_eff, mode, feedback, cut, cut_reason, pre_frame, reval, history_used, trace_w,
  trace_h, hp_eq_px, expected_phase0, s_max, n_over, hist_texels. resample fills the first fourteen in the readback block,
  next to the existing record, from values already in scope there: tag = test option fold_tag, serial = the state's serial,
  clock = params.fold_clock (RestirGiState::frames before frames++), k and k_eff and mode and feedback from this frame's
  RestirFoldFrame, cut and cut_reason from the epoch plan (reason 0 when there is no cut), pre_frame =
  params.fold_pre_frame, reval = RESTIR_F_REVAL_FRAME in params.flags (budget revalidation frames only; share mode's
  RESTIR_F_REVAL_SHARE is not this bit), history_used = use_history, trace_w and trace_h from the trace size. hp_eq_px,
  expected_phase0, s_max, n_over and hist_texels stay -1 until 12b's dump fills them. A record exists only on frames with
  the readback test option on (the existing gate), so with readback off nothing new runs per frame except the release_once
  test, the state lookup (now getptr, then operator[] only for a new view) and two summary copies.
- get_records is unchanged: stride 35, same fields and order. It now copies the list under the records mutex and builds
  its array outside the lock, as get_fold_rows does.
- Fold rows. Probe get_fold_rows() returns a PackedInt64Array, 29 values per record in this order: record (index in the
  record list), frame, tag, serial, clock, k, k_eff, mode, feedback, cut, cut_reason, pre_frame, reval, history_used,
  trace_w, trace_h, n_valid, cut_rej, had_history, refused, renewed_raw, started, carried, refused_empty, hp_eq_px,
  expected_phase0, s_max, n_over, hist_texels. get_fold_row_fields() returns those names (restir_fold_row_names), so the lab
  reads by name and a later field is appended, never inserted. restir_fold_pack_row (restir_fold_rows.cpp) builds one row:
  - n_valid = restir_fold_n_valid, the sum of all 16 entries of stats.temporal_reject. read_stats fills that histogram only
    for info words with INFO_VALID (bit 14), and TEMPORAL writes info 0 on an untraced tile and on an invalid receiver, so
    n_valid is the frame's valid traced receivers. SPATIAL rewrites bits 4 to 7 and may set bit 9, and FINAL ORs in bits 10
    to 12 and 15 and up, so bit 14 and the low four bits are TEMPORAL's. The population: receivers with geometry (w above
    0.5) on a traced tile. Sky pixels and untraced tiles are in neither n_valid nor the four counts; a ray-thinned valid
    pixel is counted. stats.geometry (get_records column 1) counts the same set from FINAL, and the row's record index is
    get_records' row index, so the lab VOIDs a row whose n_valid differs from geometry.
  - cut_rej = restir_fold_cut_rej, temporal_reject[9] (REJ_CUT). had_history = stats.had_history (INFO_HAD_HISTORY).
    REJ_CUT is given only when no history is read, so cut_rej == n_valid exactly when history_used is 0, and cut 1 implies
    history_used 0. With the probe option history off, cut is 0 and history_used is 0: the lab writes its cut column from
    cut and VOIDs any row with cut 0 and history_used 0.
  - mode is the RestirMode value: 0 B1, 1 B2, 2 U1, 3 B2k. cut_reason is the RestirCutReason value: 0 none, 1 first frame
    of a state, 2 request, 3 origin, 4 backend, 5 scene without feed, 6 scene overflow, 7 scene unlocalised, 8 Fold, 9 world. The
    plan produces 6 when the dirty feed overflows or hands over more than 64 boxes, 7 when it says boxes and gives none or a
    box that is not finite or has a negative size, and 9 when the world epoch moves. R2's own cuts (a camera jump over 25 m, a resize, an R2 refusal on the frame before) reach
    the module as history_cut and read 2, the same as a forced LightingUnits.BumpHistoryResetSerial; the rows can't tell
    them apart.
  - record is the index in the current record list. It restarts at 0 after reset_records, and every get_fold_rows call
    packs the whole list. The lab doesn't call reset_records inside a leg, keys rows by (serial, clock), and reads the
    rows once at the end of a leg.
  - refused, renewed_raw, started, carried, refused_empty: pass-stats slots 22, 24, 25, 23 and 26, read by read_stats from
    the TEMPORAL row (doubles, cast to int64; exact below 2^53).
  - renewed_raw is slot 24 as the shader writes it. The shader adds to it on every rule-1 refusal and nowhere else: the
    refusal sets the cleared flag, and a cleared pixel always takes the renew branch of rs_fold_bits. So renewed_raw equals
    refused on every frame of this design. The disjoint class "renewed" of the reader is renewed_raw − refused, which is 0
    on every row; the lab does that subtraction and asserts renewed_raw == refused, not only at least, so a later shader
    change that breaks it fails loudly (12b edits this shader). The engine exports the raw slot only.
  - The lab's identity is refused + (renewed_raw − refused) + started + carried == n_valid on a frame with K_eff above 0,
    since every valid traced receiver in TEMPORAL calls rs_fold_bits once and it adds 1 to exactly one of carried,
    renewed_raw and started. On K_eff 0 frames the four counts are 0, and on a frame without history started == n_valid;
    the lab applies the identity only when k_eff is above 0.
- Records mutex. restir_frame_records_mutex() (restir_test.h, defined in restir_gi.cpp) guards the push in resample (render
  thread) and the copy in get_fold_rows, get_records and reset_records (main thread). get_fold_rows copies the list under
  the lock and packs outside it.
- Tag rule. configure({"fold_tag": t}) sets the test option fold_tag; every record and the status key fold_tag carry the
  tag of the frame that ran. The option is read on the render thread, so the first frame with the new tag can be one frame
  after the call. The lab splits rows by tag, then drops a scenario's leading rows until its first row with cut 1.
- State serial and backend totals (restir_gi.h / .cpp). RestirGiState::serial is set from the backend's
  state_serial_next (first state 1) every time begin_frame makes a new state and its allocation succeeds: a view seen for
  the first time adds 1 to state_creates_total; a view whose state exists but is unallocated or has another trace size
  adds 1 to state_rebuilds_total. A failed allocation moves neither total nor the serial; the retry that succeeds counts
  once, as a rebuild, since the empty entry stayed in the map. A view released by the module (release, release_once, R2's free_data or RtgiPass::release) has no
  state, so its next frame is a create, not a rebuild. Every new state starts at frames 0 with a RESTIR_CUT_FIRST frame,
  so serial changes exactly where the clock v restarts. release_calls_total counts release(p_buffers) calls with a
  non-null pointer that found a state (release(nullptr), the release of everything at shutdown, is not counted).
- release_once. configure({"release_once": true}) sets a one-shot test option. The next begin_frame that passes every
  refusal test clears it, calls release(p_inputs.buffers) (the real path), adds 1 to release_tests_total and then makes the
  new state in the same call, with a new serial. It sits after all of begin_frame's refusals, so a refused frame does not
  spend the shot, and after the "no buffers" test because release(nullptr) would free every state and the core's
  pipelines. The option is a plain bool written from two threads, as the other test options are: a configure call that
  lands between the render thread's read and its clear is lost, and the lab VOIDs the release leg when
  release_tests_total did not move. With several views it releases the first view that begins a frame, and fold_tag is one
  value for the process: the 12a legs run exactly one RTGI view, and the lab VOIDs a scenario whose rows hold a serial it
  did not expect.
- Status keys (get_status): fold_feedback (last frame that ran), fold_tag (the tag that frame ran with), state_serial (its
  state's serial; 0 before any frame), state_creates_total, state_rebuilds_total, release_calls_total,
  release_tests_total.
- R2 release counters (servers/rendering/renderer_rd/environment/rtgi.cpp and rtgi.h; their own commit, CPU only, and
  this bullet and the two lines naming them below hold only while that commit is in the tree). RtgiPass gains two static
  atomic counters (SafeNumeric<uint64_t>), only ever incremented and never read by render code: release_free_data_total,
  added in RtgiBuffers::free_data after backend->release(owner), and release_pass_total, added in RtgiPass::release after
  backend->release(buffers). Both count only calls that reached a registered backend; free_data counts also when the
  module had no state for those buffers, so it is weaker proof than the module's own release_calls_total, and the lab
  reads the two deltas together. _write_status (status.json, rtgi/debug_status_path) gains the top-level keys
  rtgi_release_free_data and rtgi_release_pass. Nothing in the module reads them: the module builds and behaves the same
  without that commit, and the lab reads them from R2's status file.
  - Which path moves which counter, by reading. Forward clustered's GI block calls RtgiPass::process only when render_gi,
    the normal-roughness slice, DDGI's custom data and RtgiPass::is_enabled() (rtgi/enabled) all hold, and otherwise
    calls RtgiPass::release. So rtgi/enabled off reaches the pass path, once the view holds RTGI custom data (one
    processed frame first). RtgiPass::release ends in set_custom_data with a null Ref, which only erases the map entry;
    free_data is called only from RenderSceneBuffersRD::cleanup (the destructor and the two configure paths), and
    RtgiBuffers has no destructor that calls it. So the pass path moves rtgi_release_pass by 1 and rtgi_release_free_data
    by 0, and a SubViewport resize moves rtgi_release_free_data by 1. Whether the resize also leaves the pass counter
    still depends on DDGI's custom data surviving cleanup, which was not read.
  - How to read them. Both totals are process-wide since start and cover every viewport, while the status file is
    written per buffers: read the difference across the forced event, never the absolute value, and keep one RTGI view
    per process so no other viewport moves them. Both stay at 0 with no backend registered, so a leg with the module
    off can't use them. One RtgiPass::release call can't fire both paths, so both keys moving by 1 across one event
    means two separate releases, and the lab reports that as such instead of counting it as one clean path.
- Seeds and CPU mirrors (restir_fold_mirror.h / .cpp, pure). The doctest mirrors moved into engine functions, with the
  same arithmetic: restir_fold_pcg, restir_fold_rng_state, restir_fold_phi, restir_fold_isqrt_from, restir_fold_isqrt,
  restir_fold_draw. restir_fold_draw returns 0 for K below 2 (K² / 4 = 0), where the old mirror divided by zero; no Fold K
  reaches that. Constants: RESTIR_FOLD_C1, RESTIR_FOLD_C2, RESTIR_FOLD_DIM_RENEW (0), RESTIR_FOLD_DIM_START (1),
  RESTIR_FOLD_DETERMINISTIC_SEED (0x1234567). restir_fold_rand_seed(seed) = rtgi_pcg(seed * 0x9E3779B1 + 0x7F4A7C15), the
  seed rs_rand and rs_rand_frame hash. The mirrors exist to be compared with 12b's GPU self-test. They are never the
  source of the reader's draw.csv, phi_classes.csv or psi.csv, which are about the shader; a lab dry mode may use them
  for format checks only, in a directory of its own.
- restir_fold_other_seeds(rtgi_seed, seed_offset) lists every seed sum another rtgi_rng_state caller can use, in order:
  C1; rtgi_seed + 0 .. 7 (rtgi_rand2 in R2's MODE_RTGI, with the doubled dimension); only when seed_offset is not 0,
  rtgi_seed + seed_offset + 0 .. 7 (the module's SOURCE, which runs on params.seed); restir_fold_rand_seed(rtgi_seed +
  seed_offset) + 64 .. 575; restir_fold_rand_seed(0x1234567) + 64 .. 575. That is 1033 values at seed offset 0 and 1041
  otherwise. Both hashed blocks are always listed, since the deterministic setting can switch between them.
- Probe get_fold_seeds() returns {c1_seed, c2_seed, other_seeds (PackedInt64Array from restir_fold_other_seeds with the
  live rtgi/seed and the seed_offset test option), rtgi_seed, seed_offset, deterministic (gi/deterministic now),
  rand_seed_live (the hashed seed rs_rand uses under those settings)}. It reads rtgi/seed and seed_offset at the call,
  and R2 reads its seed through a cached setting: the lab sets both before the first frame and calls get_fold_seeds
  after the leg.
- What fires each new counter or field (no counter here is without a lab action that moves it):
  - record cut, serial, state_creates_total: the first frame of every leg (a new view is a create, a RESTIR_CUT_FIRST
    frame); record cut also on every forced cut (LightingUnits.BumpHistoryResetSerial, R2's history_cut).
  - state_rebuilds_total: the reset leg's resample path, rtgi/resolution_scale 1 to 0.5 (the trace size changes on the
    same buffers).
  - release_tests_total, release_calls_total: the reset leg's release path, configure({"release_once": true});
    release_calls_total also moves on free_data and the pass path when the view had a state.
  - rtgi_release_free_data: the reset leg's free_data path, a SubViewport resize (RenderSceneBuffersRD::cleanup calls
    free_data on every custom data, RtgiBuffers among them).
  - rtgi_release_pass: the reset leg's rtgi_pass_release path, rtgi/enabled off for 2 frames then on, after at least one
    processed frame. The counter is the proof, and the lab VOIDs that path when it does not move.
  - record pre_frame and reval: the preframe scenario (gi/revalidate budget, revalidate_period 3).
  - record feedback, mode, k_eff changes: the transitions leg (configure spatial_feedback 1 and back, gi/mode B2 to B2k,
    gi/fold_k 8 to 16).
  - record and status fold_tag: every scenario's configure({"fold_tag": t}).
  - fold rows refused, carried, started: static and pan at any K above 0; refused_empty: the thinned leg
    (rtgi/rays_per_pixel 0.5). renewed_raw moves with refused.
  - get_fold_seeds: read once per run for run.json.
- Not in 12a: item 1 and last_lit (GI's), the dump fields (hp_eq_px, expected_phase0, s_max, n_over, hist_texels stay -1
  in every 12a run; 12b's dump is what fills them, and its bars are where they are tested), the self-test, lineage.
- Doctests. tests/test_restir_fold.h ("[RestirFold] ...") now calls the engine functions in place of its own mirrors, with
  the same known answers (pcg(0) 0x07BB2FE2, pcg(1) 0xA8BEEA3C, φ(3, 5) 0x2D397518, ψ_r(3, 5, 7) 0x5072A4E0, ψ_s
  0xB99D54B9, draw(ψ_s, 8) 7, draw(ψ_s, 32) 13, KB9 at every K, the isqrt from a seed one off either way).
  tests/test_restir_fold_rows.h ("[RestirFoldRows] ..."): the constants; restir_fold_rand_seed(1) 0xF5F17964 (worked by
  hand for this commit) and (0x1234567) 0x56F88133; other_seeds at (1, 0): 1033 values, C1 first, 1 .. 8, the hashed
  blocks' ends, every value at ring distance 5 or more from C2 and C2 + 1; other_seeds at (1, 3): 1041 values with the
  module's 4 .. 11 and the offset seed hashed; isqrt exact from 0 to 4096; draw at K 0 and 1 gives 0; n_valid and cut_rej
  over a planted histogram (1 .. 16 gives 136 and 10); the 29 names in order; a planted record packs to the known row
  (renewed_raw raw, the four classes summing to n_valid); a default record packs -1 in every Fold field and 0 in every
  count.
- Known limits.
  - fold_tag is read on the render thread without a lock, as the other test options are; a tag change lands on the next
    frame that runs, and the lab drops leading rows until the first cut.
  - Records and the totals are per process; a reset_records call clears records, never totals.
  - The rows carry the module's cut reason only. R2's cut kind for the frame is not in them.
  - Whether a lab set_setting of rtgi/resolution_scale reaches R2 in the same frame (R2 reads it through a cached
    setting) was not read; state_rebuilds_total is the proof for that path, and the lab VOIDs it when it does not move.

## Fold dump and self-test

Change plan commit 12, sub-commit 12b (C12-DESIGN section 2.2): debug-only shader text, a lazily built dump shader, a
self-test shader and the probe methods that read them. With the dump off nothing a frame computes or binds changes.

- Shared Fold include (item a). shaders/restir_fold_inc.glsl holds RS_FOLD_C1, RS_FOLD_C2, RS_FOLD_DIM_RENEW,
  RS_FOLD_DIM_START, rs_fold_isqrt, rs_fold_draw and rs_fold_phi, moved word for word out of restir_gi.glsl, plus the
  two macros RS_FOLD_PSI_R(pixel, v) and RS_FOLD_PSI_S(pixel, v). restir_gi.glsl now
  has #include "restir_fold_inc.glsl" at the same place inside the TEMPORAL block (after RS_FOLD_CLOCK and RS_FOLD_K,
  which stay because they alias rf.shade_pad1 and rf.shade_pad2). The include has no bindings and needs only rtgi_pcg.
  - Route taken: the moved text is identical. glsl_builders.py inlines an include in place, without a marker line.
    The generated source string for every existing mode is the old string plus the dump lines (all inside
    #ifdef RS_FOLD_DUMP, which a preprocessor drops), the two macro definitions, and the two lines of rs_fold_bits
    that now call the macros. Each macro expands to the exact old text (rtgi_rng_state(pixel, v, RS_FOLD_DIM_RENEW,
    RS_FOLD_C2) and the same with RS_FOLD_DIM_START), so the token stream after preprocessing is the one at
    200ba76c0. The self-test's sub 2 runs the same two macros, so it checks the text TEMPORAL runs.
  - How to prove it: build 200ba76c0 and this commit and compare, per mode and variant, the preprocessed TEMPORAL text
    (glslang -E with the same defines) and the SPIR-V. Two conditions: no --generate-spirv-debug-info (with it OpLine
    and the source text move by the added lines), and both sides compiled fresh. ShaderRD keys its cache on the
    source text, so every ReSTIR variant misses the shader cache once after this commit; a compare must match
    variant and mode, never file names, and never take one side from a cache made by the other.
- Dump block (item b), all under #ifdef RS_FOLD_DUMP:
  - Binding: set 0 binding 32, std430 RestirFoldDump { uint s_max; uint n_over; uint texels; uint pad; uint starts[]; }
    rs_fold_dump, declared only when MODE_TEMPORAL and RS_FOLD_DUMP are both defined.
  - Slots: PS_FOLD_HP_EQ_PX 27, PS_FOLD_PHASE0 28 (pass-stats stride 32, row 0 TEMPORAL). They go through g_stats, so
    they are written only with F_PROBE_STATS, as every other slot.
  - History scan (s_max, n_over, texels): at the top of TEMPORAL main, before the traced-tile test, every in-grid
    thread reads hs = (rs_load(px).flags >> 8) & 0x3F from the history set (res_in, binding 7). One work-group reduction
    in shared memory, then one atomicMax and two atomicAdd per group. n_over counts hs > K − 1. Only when RS_FOLD_K is
    not 0, so at K 0 all three stay 0 and texels is 0.
  - Relock counts: in rs_temporal_pixel, for a valid receiver with hp == px: slot 27 += 1, and slot 28 += 1 when
    (RS_FOLD_CLOCK + φ(px)) mod K == 0. Only when K is not 0. hp is −1 without F_HISTORY, so cut frames add 0. In 12b
    the call sat before had was formed; since Fold check 6 (b) it sits right after had and splits the phase-0 count
    (slot 28 with had, slot 29 without; below).
  - Start histogram: inside if (RS_FOLD_K != 0), right after rs_fold_bits writes the flags, when !had (the start
    branch): s = (flags >> 8) & 0x3F after the K − 2 cap, u = (v + φ(px)) mod K with v = RS_FOLD_CLOCK (Q10),
    class c from the TEMPORAL reason, then atomicAdd(starts[(c · 64 + s) · 64 + u], 1).
  - Class codes (since Fold check 6 (b), below): 0 cut (REJ_CUT 9), 1 frame edge (REJ_FRAME 1), 2 plane (REJ_PLANE 2),
    3 normal (REJ_NORMAL 3), 4 class (REJ_CLASS 4), 5 no sample (REJ_NO_SAMPLE 10), 6 visibility (REJ_VIS 11), 7 any
    other reason (0, 5 to 8, 12 to 15); plus 8 on a pre-frame (F_PRE_FRAME), 16 classes. RS_FOLD_START_CUT 0, _FRAME 1,
    _PLANE 2, _NORMAL 3, _CLASS 4, _NO_SAMPLE 5, _VIS 6, _OTHER 7, _PRE 8, _S 64, _U 64. In 12b the codes were 0 cut,
    1 frame edge, 2 other, plus 3 on a pre-frame (6 classes).
- Dump shader objects (item c). The dump is its own RestirGiShaderRD per variant (RestirFoldDumpObjects, heap, made on
  the first dump frame and never at start), with the core's general defines for that variant plus #define RS_FOLD_DUMP
  and two modes: MODE_TEMPORAL and MODE_TEMPORAL + SKY_REREAD (SW and HW only; VARIANT_NONE has the first only). It
  has its own pipelines and its own dispatch (_dispatch_dump), so PassMode, PASS_MAX, the pipeline table, the marker
  names and _dispatch are unchanged. Set 0 is the TEMPORAL list plus binding 32, set 1 the ray scene, set 2 (sky mode)
  the shade set, each through UniformSetCacheRD with the dump shader's RID. Timestamps "ReSTIR Temporal Fold Dump" and
  "ReSTIR Temporal Sky Fold Dump". If the dump shader, a pipeline or a set is not valid, the normal TEMPORAL pass runs
  and the frame reads as not dumped.
- Dump buffer and read-back (item d). RestirGiResources::fold_dump, 262,160 bytes (16-byte header + 16 · 64 · 64
  words; 98,320 bytes with 6 classes in 12b), made by ensure_fold_dump(true) and freed by ensure_fold_dump(false) or free_all; alloc_log entry "fold_dump".
  run_frame clears it (buffer_clear) just before the dump dispatch and sets RestirGiResources::fold_dump_ran only when
  the dump pipeline was bound and dispatched. read_fold_dump reads header and starts with buffer_get_data.
  read_stats adds v[27] and v[28] to RestirFrameStats::fold_hp_eq_px and fold_phase0 (always 0 without the dump).
- Backend. resample calls ensure_fold_dump(fold_dump and readback), so no buffer exists unless the lab asks. The dump is
  read back only in the readback block: when fold_dump_ran, record.fold.hp_eq_px and expected_phase0 come from slots 27
  and 28, s_max, n_over and hist_texels from the header, and the starts are added to a module-wide accumulator
  (restir_fold_starts, uint64 × 65,536, under restir_fold_starts_mutex; restir_test.h). Status: fold_dump_active (this
  frame's dump really ran), fold_dump_frames_total (frames where it ran).
- Self-test (item e). shaders/restir_fold_test.glsl, its own ShaderRD (RestirFoldTestShaderRD), one variant, built on
  the first run_fold_test call. Push constant FoldTestParams { sub, k, count, v0, grid_w, grid_h, stride, frames }
  (8 uint, 32 bytes); set 0 binding 0 FoldTestOut { uint words[]; }. Work group 64 × 1 × 1; dispatch x =
  min(groups, 1024), y = ceil(groups / x); thread t = group.y · x · 64 + global.x.
  - sub 0 draw: t < count, words[t] = rs_fold_draw(t, k). count words.
  - sub 1 φ classes: t < grid_w · grid_h, px = (t mod grid_w, t / grid_w), atomicAdd(words[φ(px) mod k], 1). k words.
  - sub 2 ψ: t < count · frames, i = t / frames, v = v0 + t mod frames, pixel i · stride in row order;
    words[t] = (φ mod k) | (ψ_r mod k) << 8 | draw(ψ_s, k) << 16. count · frames words.
  - sub 3 isqrt: t < count, a miss of s² ≤ t < (s + 1)² adds 1 to words[0] and atomicMin(words[1], t). 2 words,
    starting {0, 0xFFFFFFFF}.
  - Arguments are checked by restir_fold_test_invalid: sub 0 to 3, k a power of two from 4 to 64, count above 0 (not
    sub 1), grid above 0 (subs 1, 2), frames and stride above 0 and the last pixel in the grid (sub 2), at most 2^24
    threads and 2^22 words. The CPU mirror (restir_fold_test_mirror) runs the same four sub-tests;
    restir_fold_test_compare counts differing words plus any size difference.
  - For the reader's files the lab uses: draw.csv sub 0 with count 3K²/4; phi_classes.csv sub 1 at the trace grid
    size (Q9); psi.csv sub 2 with count 2048 and the stride and frames it picks; isqrt sub 3 with count 2^24.
- Probe methods.
  - configure({"fold_dump": true}) turns the dump on and also turns readback on; false turns it off (readback stays).
  - run_fold_test(args: Dictionary) posts the test to the render thread; fold_test_ready() and
    get_fold_test_result() read it. mirror_fold_test(args) runs the CPU mirror on the calling thread. Args keys: sub, k,
    count, v0, grid_w, grid_h, stride, frames (defaults 0, 8, 0, 0, 0, 0, 1, 1). Result: {source "gpu" or "mirror", ok,
    error, sub, k, count, v0, grid_w, grid_h, stride, frames, word_count, words (PackedInt64Array), mirror_mismatches,
    first_mismatch}; for the GPU run mirror_mismatches is compare(GPU, mirror) and first_mismatch its first index (−1
    when none); for the mirror it is 0.
  - get_fold_starts() returns the 65,536 accumulated start counts (index (c · 64 + s) · 64 + u; zeros before any dump
    frame); get_fold_starts_frames() the number of dump frames added since the last reset; reset_fold_starts() clears
    both. reset_records does not.
  - mirror_fold_starts(info: PackedByteArray, width, k, clock, pre_frame) returns the 65,536 counts a cut frame must
    give: for every texel of the info image with the valid bit (bit 14), s = draw(ψ_s(px, clock), k), capped at
    K − 2 on a pre-frame, u = (clock + φ(px)) mod K, class 0 (8 on a pre-frame). CPU only. An empty array means the
    arguments were refused (size not width × height × 4 bytes, k not a power of two from 4 to 64, clock below 0).
    run_case returns info, so the known-answer cut case compares its fold_dump starts with this count by count.
  - Self-test arguments outside 0 to 2^32 − 1 give ok false with error "argument out of range" (no wrap). Without a
    rendering device run_fold_test gives ok false with error "no rendering device"; mirror_fold_test still runs.
  - run_case reads the fold_dump test option (configure, not the case file). When it is on, run_case sets
    F_PROBE_STATS, makes the dump buffer in the case's own resources and returns result["fold_dump"] = {active, k_eff, clock, n_valid, cut_rej, had_history, hp_eq_px, expected_phase0,
    s_max, n_over, texels, starts}; the last four are −1, −1, −1 and an empty array when the dump did not run.
- What fires each new key or counter:
  - fold_dump_active, fold_dump_frames_total, record hist_texels: any 12b leg with configure({"fold_dump": true}) and
    K above 0; hist_texels = trace_w · trace_h on every such frame.
  - record s_max, n_over: static, pan, dolly and cut scenarios (check 3); n_over above 0 only in the known-answer case
    P5 (one texel at s = K).
  - record hp_eq_px, expected_phase0: static (every receiver relocks) and pan (check 6).
  - get_fold_starts (12b's 6 classes; the 16 classes since Fold check 6 (b) are below): the cut scenario (class 0),
    pan frame edges (class 1), other starts in pan and dolly (class 2), the preframe scenario (class 3 only: its pose
    is static, so a pre-frame there starts pixels by a cut). Classes 4 and 5 need a frame edge or a disocclusion on a
    pre-frame; no planned leg makes one, and the reader needs only the sum of 3 to 5. If revalidation is on a budget,
    a pre-frame can fall on another scenario's first frame and move its cut starts from class 0 to class 3; the lab
    reads the pre_frame column before it names a class.
  - K 0 with the dump on (scan and relock return early, texels 0, slots 27 and 28 zero): the lab's dump-on identity
    case adds one K 0 frame.
  - fold self-test: run once per run per K for draw.csv, phi_classes.csv, psi.csv and the isqrt sweep.
- Doctests. tests/test_restir_fold_dump.h ("[RestirFoldDump] ..."): start classes and the histogram index ((5, 63, 63)
  is 24,575, dump 98,320 bytes, binding 32, slots 27 and 28; Fold check 6 (b) changed these, below); compare finds a planted difference (one word, two words,
  a short array); the draw weights over one period at every K (1 at s = 0, 2s + 1 below K/2, K from K/2); the isqrt
  sub over 2^24 gives {0, 0xFFFFFFFF}; φ counts sum to the grid and a ψ word packs its three fields; argument checks;
  the start accumulator adds two frames, counts them and ignores a short one; the cut-frame start mirror puts every
  valid texel in class 0 (3 on a pre-frame), none past K − 1 or past the pre-frame cap, skips texels without the valid
  bit and refuses a K that is not a power of two. tests/test_restir_fold.h and test_restir_fold_rows.h
  are unchanged.
- Rules for the lab that reads these.
  - A counted row with s_max, n_over or hp_eq_px at −1, or with hist_texels not trace_w · trace_h, is VOID: the dump
    did not run on that frame. The frozen reader would read −1 as a value (n_over −1 as an over frame, hp_eq_px −1 as
    a negative share), so such rows never reach s_max.csv, relock.csv or preframe.csv.
  - Start windows are proved by identities, VOID on a miss: over a window, the sum of get_fold_starts over all
    classes equals the sum of the rows' started over the same frames, and get_fold_starts_frames equals the number of
    rows; on a cut row the class 0 (or 3; 8 since Fold check 6 (b)) total equals that row's n_valid and started. Starts are added before the
    row is pushed, so once row N is visible frame N's starts are in.
  - The first dump frame of a variant compiles the dump shader and its pipelines on the render thread and can take
    seconds (hardware variant most). The lab warms the dump once before the counted frames and takes no timing from
    a dump frame. run_fold_test also runs the CPU mirror on the render thread: sub 3 at 2^24 is 16 million roots.
  - Dump on against dump off (identity case): hashes first. The dump pass is a different pipeline, so a driver may
    schedule floats differently; a hash miss falls back to the float compare at the registration bar before it is
    called a failure.
  - In 12b expected_phase0 (slot 28) counted every valid receiver with hp == px at phase 0; refused counts only
    receivers that reached the refusal test. A few pixels of difference on a judged frame are history rejections
    (plane, normal, no sample, epoch, dirty), not a dump fault. Since Fold check 6 (b) slot 28 counts only those with
    history, and the rest go to slot 29 (phase0_start).
- Known limits.
  - The dump runs only with readback on. Starts accumulate across every view and leg in the process; the lab calls
    reset_fold_starts at the start of a leg and runs one RTGI view.
  - ensure_fold_dump adds and removes its alloc_log entry on the render thread while get_status may read the log on
    the main thread, as the debug texture's entry already does. The lab toggles fold_dump only between legs.
  - run_fold_test uses the probe's own core (the one run_case uses), never the backend's.
  - A dump that fails to build is tried once per variant and core; fold_dump_active stays false after that.

## Fold lineage

Change plan commit 12, sub-commit 12c (C12-DESIGN section 2.3): a debug-only lineage per pixel and a refused-id image, so
the lab can count how many receivers clear one lineage on one frame (fanin.csv, check 12, and the 4d bar). With
lineage off nothing a frame computes or binds changes, and the 12b dump objects are unchanged.

- Shader objects. Lineage is a third set of TEMPORAL objects next to the core's and the dump's: a RestirGiShaderRD per
  variant (RestirGiCore::lineage_objects, the same RestirFoldDumpObjects type, heap, made on the first lineage frame),
  with the core's general defines plus "#define RS_FOLD_DUMP" and "#define RS_FOLD_LINEAGE", and the same two modes as
  the dump (MODE_TEMPORAL; MODE_TEMPORAL + SKY_REREAD for SW and HW). So the lineage pass is the dump pass plus the
  lineage writes: binding 32, the history scan, slots 27 and 28 and the start histogram all run in it.
  - C12-DESIGN 2.3 named a mode pair PASS_TEMPORAL_LINEAGE and PASS_TEMPORAL_SKY_LINEAGE. 12b did not add PassMode
    values for the dump (it has its own objects and _dispatch_dump), so 12c follows the code: _ensure_dump and
    _dispatch_dump take a lineage flag, and PassMode, PASS_MAX, the pipeline table and the marker names are unchanged.
    The dump objects' define string stays "\n#define RS_FOLD_DUMP\n" byte for byte. Timestamps "ReSTIR Temporal Fold
    Lineage" and "ReSTIR Temporal Sky Fold Lineage".
  - All new GLSL is under #ifdef RS_FOLD_LINEAGE (constants in restir_gi_inc.glsl sit inside the existing
    #ifdef RS_FOLD_DUMP block; the image declarations need MODE_TEMPORAL, RS_FOLD_DUMP and RS_FOLD_LINEAGE). No existing
    variant or 12b dump object defines it, so their preprocessed token streams are the ones at 760782cd8. The same
    proof as 12b applies (no SPIR-V debug info, both sides compiled fresh, matched by variant and mode); the shader cache
    misses once for every ReSTIR object because the source text changed.
- Bindings (set 0, TEMPORAL lineage objects only):
  - 33 rs_fold_lineage_in, rg32ui uimage2D, readonly: fold_lineage[hist], the history side.
  - 34 rs_fold_lineage_out, rg32ui uimage2D, writeonly: fold_lineage[hist ^ 1].
  - 35 rs_fold_refused_id, r32ui uimage2D, writeonly.
  - The two lineage images ping-pong with the reservoir index: TEMPORAL reads reservoir[hist] and lineage[hist], and
    writes reservoir[hist ^ 1] and lineage[hist ^ 1]. history_index is always 0 or 1, so two images are enough.
- Lineage word (rg32ui). x = birth pixel index + 1 = y · trace_w + x + 1, so 0 means "no lineage known"; y = the birth
  clock v (RS_FOLD_CLOCK). Per in-grid thread, in TEMPORAL:
  - not traced, no valid receiver, or K 0: word (0, 0), id 0.
  - start (!had): word (own pixel + 1, v), id 0.
  - carried (had and not cleared): word = lineage_in at hp, id 0.
  - refusal (cleared, the same branch that adds 1 to PS_FOLD_REFUSED): id = lineage id of lineage_in at hp; then the
    renewal writes (own pixel + 1, v).
  - Every in-grid texel of lineage_out and refused_id is written on every lineage frame, so neither holds a stale word.
- Lineage id (r32ui, C12-DESIGN calls it "hash"). id = ((v_birth & 0xFF) << 24) | (pixel + 1), and 0xFF000000 when the
  word's pixel field is 0 or above 0xFFFFFF (an unknown lineage). It is never 0, a real id never has its low 24 bits
  zero, and it is exact, not a hash: inside one frame every refused lineage was born at most K + 1 ≤ 65 frames earlier
  (a carry adds 1 to s and refuses at s + 1 ≥ K, or K − 1 on a pre-frame), so 8 clock bits keep two lineages of one
  pixel apart, and two different refused lineages never share an id. The price is the trace limit: lineage runs only
  when trace_w · trace_h ≤ 0xFFFFFF (16,777,215 texels); above it ensure_fold_lineage makes nothing,
  fold_lineage_active stays false and status reports reason code 2 (below). CPU mirror restir_fold_lineage_id,
  restir_fold_lineage_pixel.
- Resources. RestirGiResources::fold_lineage[2] (R32G32_UINT, trace size) and fold_refused_id (R32_UINT), made by
  ensure_fold_lineage(true) and freed by ensure_fold_lineage(false), free_fold_lineage or free_all (the same places the
  12b dump buffer is freed); alloc_log entries "fold_lineage 0", "fold_lineage 1", "fold_refused_id". The backend calls
  ensure_fold_lineage(fold_lineage and fold_dump and readback) right after ensure_fold_dump. run_case never makes them,
  so the harness runs the plain dump.
  - The three images count as present only when all three RIDs are valid. If any one fails to create, all three are
    freed, RestirGiResources::fold_lineage_failed is set, lineage stays off with reason code 3, and one warning is
    printed (WARN_PRINT_ONCE). While the flag is set nothing is created again, so nothing logs per frame. The flag
    clears in free_all (resize or state rebuild) and when lineage is no longer wanted.
  - RestirGiResources::fold_lineage_refusal is set by every ensure_fold_lineage call (0, 2 or 3) and set to 3 by
    run_frame when the lineage dispatch did not run (lineage objects failed to build). The backend copies it to
    RestirGiSummary::fold_lineage_refusal after run_frame.
- Stale words. RestirGiResources::fold_lineage_written holds the index of the lineage image the last lineage dispatch
  wrote (−1 for none). Before a lineage dispatch, if it is not hist, run_frame clears lineage[hist] to 0 (texture_clear),
  so every carried word reads as unknown, never as a word from an older frame. It is −1 after creation, after
  clear_history, after a frame without the lineage pass (lineage off, the dump object ran instead, or the plain TEMPORAL
  ran) and after free; a frame where history_index did not become the written image (spatial feedback on) also misses.
  - First frame after a cut: F_HISTORY is off, every valid receiver starts and writes its own word, nothing is read.
  - Resize or resample: the state is rebuilt (allocate calls free_all), the images are new, the first frame is a cut.
  - Change of K, of mode or of feedback: restir_fold_cut cuts on any k_eff change, so the same as a cut. While k_eff is
    0 (feedback on) the lineage pass writes word 0 everywhere, so if K came back without a cut the carried words
    would read as unknown.
  - Lineage turned on mid-scenario: until the next cut, pixels carried from before have word 0 and their refusals
    count as unknown (below), never in a family.
- Read-back and families. When the lineage pass ran and readback is on, the readback block reads fold_refused_id with
  texture_get_data (trace_w · trace_h · 4 bytes, 921,600 at 640 × 360; RestirGiCore::read_fold_refused_ids hands back
  the read-back bytes and the texel count, with no second copy) and calls restir_fold_families_add(options.fold_tag,
  ids, texels) on that buffer (the tag passed is record.fold.tag, the same value). It groups the frame's ids outside any lock, then, under restir_frame_records_mutex (the
  lock the record push takes), adds them to the accumulator of that tag: restir_fold_family_tags, HashMap tag →
  RestirFoldFamilyTag {sizes: HashMap size → count, totals: {frames, refusals, unknown, families}} (restir_test.h).
  - It returns the frame's {refusals, unknown, families}, which go into the row's RestirFoldRecord fields
    lineage_refusals, lineage_unknown and lineage_families. They are −1 on a row without the lineage pass (or whose
    read-back failed), like s_max, n_over and the other dump fields. They are row fields 29 to 31 of get_fold_rows,
    named in get_fold_row_fields; RESTIR_FOLD_ROW_FIELDS was 32 in 12c (42 since Fold check 6 (b), fields 32 to 41
    after these). run_case's fold_dump dictionary does not carry them.
  - A frame's families are added before its row is pushed, and the row carries the tag, so the families of a tag cover
    exactly the lineage rows of that tag.
  - restir_fold_families(ids, count, r_sizes) (restir_fold_mirror.cpp, pure): skips 0; counts every other id as a
    refusal; an id with its low 24 bits zero is unknown and is not grouped; equal remaining ids form a family; for each
    family r_sizes[size] += 1. Returns {refusals, unknown, families}. Families are per frame: in Fold every descendant
    of one lineage carries the same s, so a lineage is refused on one frame only, and per-frame grouping is the toy's
    "receivers clearing one lineage on one frame" (RestirToyFold Families).
- Probe and status.
  - configure({"fold_lineage": true}) turns lineage on only if fold_dump is already on (or set earlier in the same
    call: fold_dump is read first). Otherwise lineage stays off and RestirTestOptions::fold_lineage_refusal is code 1;
    a later accepted fold_lineage key sets it back to 0. configure({"fold_dump": false}) also turns lineage off.
  - Reason codes (RestirFoldLineageRefusal, restir_fold_mirror.h): 0 none; 1 lineage asked without the dump; 2 trace
    grid above 16,777,215 texels; 3 lineage images or lineage shader objects not available. The options hold an int,
    not a String, so configure and get_status share no string. get_status reports the options code when it is not 0,
    else the render side's code from the last frame, and builds the text with restir_fold_lineage_refusal_text.
  - get_fold_families(tag): PackedInt64Array of (family_size, count) pairs for that fold tag, sizes ascending.
    get_fold_family_totals(tag): {frames, refusals, unknown, families} for that tag. A tag with no lineage frame gives
    empty pairs and all zeros, frames 0. reset_fold_families() clears every tag. reset_records and reset_fold_starts
    do not touch families, and reset_fold_families does not touch records or starts.
  - Status: fold_lineage_active (this frame's lineage pass really ran), fold_lineage_frames_total,
    fold_lineage_refusal (text, empty for code 0) and fold_lineage_refusal_code (int).
- Doctests. tests/test_restir_fold_lineage.h ("[RestirFoldLineage] ..."): bindings 33 to 35, the pixel word and id
  packing (unknown for pixel 0 and above the mask, the texel limit); the known answer {5, 5, 5, 7, 0, 7, 9} gives
  sizes {3: 1, 2: 1, 1: 1}, 6 refusals, 3 families, and twice gives {3: 2, 2: 2, 1: 2}; unknown ids are counted and not
  grouped; ids over a 64 × 36 grid and 66 consecutive clocks are all distinct; the accumulator adds frames, counts an
  empty frame and sums totals; per tag, {5, 5, 7} on tag 1 gives pairs (1, 1), (2, 1), refusals 3, families 2, and
  {9, 9, 9} on tag 2 gives (3, 1), refusals 3, families 1, an unknown tag gives zeros, and reset clears both.
  tests/test_restir_fold_rows.h checks the 32 row names and the −1 defaults of the three lineage fields.
- Rules for the lab that reads these.
  - Give each case of a lineage leg its own fold_tag, never reused in the process unless reset_records and
    reset_fold_families both ran. There is no reset at the cut: the families of a tag are read after all of the case's
    rows are in. The tag added to is the row's own tag (record.fold.tag, read once from options.fold_tag), so a
    configure between the two cannot split a frame.
  - A lineage window is proved by identities, VOID on a miss: get_fold_family_totals(tag).frames equals the number of
    rows of that tag with lineage_refusals ≥ 0, refusals equals the sum of refused over those rows and also the sum of
    their lineage_refusals, unknown equals the sum of their lineage_unknown, and families the sum of their
    lineage_families. unknown is 0 when the tag's rows start at or after a cut that lineage saw. If the lab wants only
    the rows from the cut on, it sums the per-row fields over those rows; the tag's pairs then cover the whole tag.
  - fold_lineage_active must be true on every row of a lineage window; the first lineage frame of a variant compiles
    the lineage shader on the render thread (seconds), so the lab warms it before the counted frames.
  - fanin.csv rows: case, family_size, count with count = the number of families of that size, straight from the
    pairs of the case's tag. The engine does not know the case; the lab maps tags to cases.
- Known limits.
  - Families accumulate per tag over every view and every lineage frame in the process, cut frames included (they add
    no refusals).
  - The read-back stalls the render thread on every lineage frame (texture_get_data); no timing comes from a lineage
    leg.
  - ensure_fold_lineage edits alloc_log on the render thread, as ensure_fold_dump does; the lab toggles lineage only
    between legs.
  - Lineage is not in run_case or the case files; P7 (12b against 12c, lineage off) uses the existing paths.
  - A lineage object that fails to build is tried once per variant and core; the dump object then runs in its place,
    fold_lineage_active stays false and status reports reason code 3.

## Fold check 6 (b): gated phase-0 counts and start reasons

Registered 14 Nov 2026 before any code (the Fold check 6 (b) registration). Dump path only: every change is
under #ifdef RS_FOLD_DUMP or in code that runs only when the dump ran, so the plain TEMPORAL variants keep their
preprocessed token stream (the shader cache misses once, as the source text changed).

- Slots (pass-stats row 0, stride 32 unchanged):
  - 27 PS_FOLD_HP_EQ_PX: unchanged, every valid receiver with hp == px and K not 0, with or without history.
  - 28 PS_FOLD_PHASE0: a phase-0 ((RS_FOLD_CLOCK + φ(px)) mod K == 0) hp == px receiver with had (c[1].present or
    cleared). The rs_fold_dump_relock call moved from just before `bool had` to just after it and takes had as its
    third argument. Nothing between the two places returns or changes px, hp, c[1] or cleared, so slot 28 + slot 29
    equals 12b's slot 28 on every frame.
  - 29 PS_FOLD_PHASE0_START (new): the same phase-0 hp == px receivers without had.
  - 30 PS_FOLD_REFUSED_OFF (new): in the rule-1 refusal branch (where slot 22 PS_FOLD_REFUSED adds 1), receivers whose
    own pixel is not at phase 0: (RS_FOLD_CLOCK + φ(px)) mod K != 0. No hp == px condition.
  - C++: RESTIR_FOLD_SLOT_PHASE0_START 29, RESTIR_FOLD_SLOT_REFUSED_OFF 30; read_stats adds them to
    RestirFrameStats::fold_phase0_start and fold_refused_off. The static_assert checks slot 30 against the stride.
- Start classes: CUT 0, FRAME 1, PLANE 2, NORMAL 3, CLASS 4, NO_SAMPLE 5, VIS 6, OTHER 7, plus PRE 8 on a pre-frame,
  16 classes (RESTIR_FOLD_START_CLASSES). Reason to class: 9 CUT, 1 FRAME, 2 PLANE, 3 NORMAL, 4 CLASS, 10 NO_SAMPLE,
  11 VIS, every other code (0, 5, 6, 7, 8, 12, 13, 14, 15) OTHER, so no start is lost. rs_fold_start_class (shader)
  and restir_fold_start_class (restir_fold_mirror.cpp) hold the same table; reason codes RESTIR_FOLD_REJ_*_CODE.
- Start buffer: 16 · 64 · 64 = 65,536 words after the 16-byte header, RESTIR_FOLD_DUMP_BYTES 262,160. The shader's
  starts[] is a runtime array; storage_buffer_create, buffer_clear, read_fold_dump, the module accumulator,
  get_fold_starts, mirror_fold_starts and run_case's starts all size from RESTIR_FOLD_START_COUNT or
  RESTIR_FOLD_DUMP_BYTES. Index (c · 64 + s) · 64 + u as before.
- restir_fold_start_sums(starts, sums) (restir_fold_mirror.cpp, pure): eight per-class totals over s and u, class
  c + 8 added into c. False and all −1 when the array is not 65,536 words.
- Rows: RESTIR_FOLD_ROW_FIELDS 42. Fields 0 to 31 are unchanged (expected_phase0 stays field 25 and now carries the
  gated slot 28). New: 32 phase0_start, 33 refused_off, 34 start_cut, 35 start_frame, 36 start_plane, 37 start_normal,
  38 start_class, 39 start_no_sample, 40 start_vis, 41 start_other. All −1 unless the frame's dump was read, like
  s_max; the start sums are that frame's dump starts (pre-frame twins folded in), not the accumulator.
- run_case's fold_dump dictionary adds phase0_start and refused_off (from the stats, like hp_eq_px).
- Identities for the lab: hp_eq_px ≥ expected_phase0 + phase0_start; the eight start sums add up to the row's
  started on a dump row (every !had start is in exactly one class); refused_off ≤ refused.
- Doctests: test_restir_fold_dump.h checks the class table for every reason 0 to 15, plain and pre-frame, the class
  ids, (15, 63, 63) = 65,535, 65,536 starts, 262,160 bytes, slots 27 to 30, the cut mirror in class 0 (8 on a
  pre-frame), and the start sums (twins folded, a short array refused). test_restir_fold_rows.h checks the 42 names,
  the −1 defaults of fields 24 to 41 and the packing of fields 32 to 41.
