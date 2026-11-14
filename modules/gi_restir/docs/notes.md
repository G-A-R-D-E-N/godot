# gi_restir module notes (skeleton, ADR-PHASE6 lane S3)

Status: skeleton only. No ReSTIR algorithm code is in this module yet (lane S4 adds it).
Default: off at build time (config.py is_enabled returns False) and off at run time (every restir setting false or neutral).

## What the module does now

- SERVERS level: registers the RtgiResampleBackend (RestirGiBackend) with RendererRD::GIModuleHooks, and creates the
  probe singleton GiRestirProbe. The backend is always registered (run-time switching stays possible) and answers
  is_active() from rendering/global_illumination/restir/gi/enabled. The path tracer backend is not part of this line. Core treats a registered backend
  with is_active() false as no backend, so with the settings at their defaults the frame is the one rendered with
  no module. With a setting on, the backend is active but still refuses every frame (no-op): core then forces the
  RTGI trace for it, runs the plain reconstruction and counts a refusal, which is not identical to the module-off frame.
- SCENE level: registers the ADR-PHASE6 8.1 settings under rendering/global_illumination/restir/.
- Uninitialise (SERVERS): release_all, unregister_all, remove the probe singleton, free the two objects.

## Settings (ADR 8.1)

| Name (under rendering/global_illumination/restir/) | Type | Default |
|---|---|---|
| gi/enabled | bool | false |
| gi/mode | enum B1,B2,U1 | B1 |
| gi/spatial_neighbors | int 0..8 | 3 |
| gi/spatial_radius | int 1..128 | 24 |
| gi/spatial_passes | int 1..2 | 1 |
| gi/m_temporal_cap | int 1..256 | 20 |
| gi/m_max | int 1..512 | 32 |
| gi/n_target | int 1..64 | 8 |
| gi/visibility_reuse | bool | false |
| gi/fresh_lo | bool | false |
| gi/blocked_keeps_coverage | bool | false |
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

The numeric defaults other than the bools are the ADR values. They have no effect while the matching enabled
setting is off. Every description starts with "Experimental.". Godot shows project-setting help from
doc/classes/ProjectSettings.xml (core), which must not name the module, so the descriptions are kept in
restir_settings.cpp and reach the probe status only. The editor settings page does not show them.

## Core include shim (ADR 7.3)

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
  include closure of the six core files (nested includes included, thirdparty skipped), for the shim and the fallback.
- Builder: SCsub uses RD_GLSL, guarded by "RD_GLSL" in env["BUILDERS"] like the core shader SCsubs. ADR 7.2 names
  GLSL_HEADER; that line is superseded.
- Placeholder shader: shaders/restir_shim_check.glsl includes the shim so the engine build proves the includes
  resolve. It is never compiled at run time. restir_probe.cpp includes its generated header, so the header is built in
  every build that builds the module (including custom_modules builds).
- SCsub loads restir_shim.py by file path (importlib), not through sys.path.

## Build cases (ADR 7.4)

- a: module absent. int4m-1 snapshot (same engine commit, no module files).
- b: in tree, module_gi_restir_enabled default (no).
- c: in tree, module_gi_restir_enabled=yes.
- d: custom_modules= folder on E: with a copy of this module (identical sources; the copy is outside the tree).
- e: D3D12. The module has no hardware variant and no D3D12-specific source.

## Probe

- Engine singleton GiRestirProbe, method get_status. Keys: module, rtgi_registered, rtgi_name, rtgi_active,
  rtgi_status, settings (name -> value, description, internal, in
  declared order).
- Game probe: opencw/tools/gi/GiRestirProbe.cs and gi_restir_probe.tscn (headless).

## Contracts for lane S4

- release(nullptr) is called twice at shutdown: once by the renderer (release_all before the device goes) and once by
  this module's uninitialise (release_all, then unregister_all). Backends must make release(nullptr) idempotent and
  must not touch the device after the first call.
- is_active() runs on the render thread every frame: no allocation, no GPU work, no locks.
- gi/deterministic and gi/dump_path are internal settings: they do not appear in the editor.

## Review notes not changed

- The review items N10, N13 and N15 of the S3 ruling were "note only"; their text was not available to lane S3F, so
  they are listed here without a description and no change was made for them.

## Provenance

- No paper, SDK or third-party code was read or used in this lane. Sources consulted: the core hook headers
  (servers/rendering/renderer_rd/environment/gi_module_hooks.h and .cpp), SConstruct module discovery,
  glsl_builders.py include resolution, project_settings.h, and the module precedents modules/gi_hook_probe and
  modules/lightmapper_rd.

## Lane S4: ReSTIR GI core (gi backend "restir_gi")

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
(pairwise MIS, symmetric accept rule, visibility both ways). A neighbour whose sample is invalid for the receiver
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

Truncation counters (S4F, F3) and clamp share (F4): the temporal and spatial passes write PS_STRIDE = 16 floats per 8x8
group per pass into a pass statistics buffer (3 slots: temporal, spatial 1, spatial 2; cleared every frame). Per frame:
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

## Lane S5: invalidation and R2 integration (ADR 5.9, D7)

Planner: restir_epochs.h/.cpp, restir_plan_epochs() decides per frame from the history flags, the previous and current GIEpochs::Snapshot and the dirty feed
(RtgiFrameInputs.dirty_source, the GIEpochs singleton core sets; no core accessor was added). Order: no history (FIRST), core history_cut (REQUEST; camera cut,
resize and backend switch reach the module only through it), hardware change (BACKEND), origin epoch change (ORIGIN), then a scene epoch change. On RT GI's R2 line there is no
dirty feed yet (RtgiFrameInputs.dirty_source is null and GIEpochs has no feed API), so every scene epoch change is a SCENE_NO_FEED cut and clears everything.
SCENE_OVERFLOW and SCENE_UNLOCALISED stay defined for the feed that GI adds later. Boxes are world space; the module subtracts
RayScene::get_anchor() to get the anchor-relative space of the geometry. State counters: cut_counts per reason, dirty_frames; status keys cut_reason, dirty_boxes,
scene_changed, light_changed, sky_changed, origin_changed, light_cap, light_frames_left.

Dirty test (restir_gi.glsl rs_dirty_hit): per reprojected history pixel with a stored sample, the slab test of the segment from the previous receiver surface point
q.pos to the stored sample point x_s (to q.pos + dir * sky_distance for a sky sample) against every box grown by dirty_margin 0.01 m. A hit gives reject code 6 (REJ_DIRTY)
with the history unused: M = 0 from it and a fresh canonical only; the pixel keeps INFO_HAD_HISTORY. Epoch tags: the stored tag is compared with prev_epoch_tag (the
previous frame epoch_tag), reject code 5.

Light epoch (Experimental): a light epoch change sets m_light_frames frames in which m_temporal_cap and m_max are capped at m_light, so the stored M is at most m_light
for exactly that many frames (defaults 2 and 4). It is a soft cap, not a cut.

Sky epoch: nothing is cleared. The sky radiance is core-owned (ddgi_trace.glsl sky_radiance_in, set 2 and push constants); the ray scene carries no sky, so a module
cannot re-read the sky by direction. A MISS_SKY sample therefore keeps its stored Lo until it is replaced; this is measured lag, not a re-read.

## Port onto RT GI's R2 line

- Rays: ray_scene_trace with DI-1's seed argument. Each call site hashes the pixel, the frame and its own odd salt.
- Backface: a hit is shaded when it faces the ray, or when its material is two-sided or a blend material, as in ddgi_trace.glsl shade_hit.
- Hardware ray scene: when RayScene::uses_hardware() is true the module uses VARIANT_HW (RAY_SCENE_HW and GL_EXT_ray_query, the ray scene's own bindings
  through append_uniforms). If that variant or one of its pipelines cannot be made, the module refuses the frame and turns the hardware backend off for the ray
  scene, as DDGI does.
- No cloud shadow and no alpha ray scene on this line; the shade passes bind set 2 bindings 0 and 3 to 8 only.
- Output route: only gi/denoise = r2 is used by RTGI. A frame with gi/denoise off is counted as a refusal and RTGI runs on its own candidates.
