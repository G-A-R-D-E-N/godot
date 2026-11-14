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

- release(nullptr) is called at shutdown only by this module's uninitialise (release_all, then unregister_all); the
  renderer never calls release_all. Backends must make release(nullptr) idempotent and must not touch the device after
  the first call.
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
  word can read up to about 2x p0(w); elsewhere it is negligible (RT GI's review of 998e583da).
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
  would avoid the extra jump frames (RT GI's review of 9ec4439b4).
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
  spheres are events; slots after them are for dirty-box spheres (centre, half diagonal, since_last UINT32_MAX), none on this line
  because the module's planner gets no dirty boxes (scene changes still cut). RestirFrame stays 400 bytes. The CPU side packs it in
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
- Later item (MC #131, 14 Nov): R2's tracker fires two events per lamp change, the old and the new state
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

## R2 side: module output, route coverage, tile memory (change plan commit 10)

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
