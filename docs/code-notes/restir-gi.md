# ReSTIR GI (`modules/gi_restir`)

### `modules/gi_restir/config.py`

`modules/gi_restir/config.py` builds the module in any build with a RenderingDevice (`can_build` returns
`env["rendering_device"]`; `configure` is empty; `get_doc_classes` returns `[]`, `get_doc_path` returns
"doc_classes"; there is no `is_enabled`). The module is reservoir resampling of the ray-traced diffuse pass: temporal
and spatial reuse of each pixel's traced sample, feeding the RTGI denoiser. The backend is inactive until
rendering/global_illumination/restir/gi/enabled is set; the other settings then act at their defaults.

### `modules/gi_restir/register_types.cpp`

`modules/gi_restir/register_types.cpp` wires the module into `RendererRD::GIModuleHooks`.

- SERVERS level: `GDREGISTER_CLASS(RestirProbe)`, creates a `RestirGiBackend` and a `RestirProbe`, registers the
  backend with `register_rtgi_backend`, and adds the probe as the Engine singleton `GiRestirProbe`. The backend is
  always registered; `is_active()` returns `restir_gi_enabled()`, a cached setting read.
- SCENE level: `restir_register_settings()` registers every setting under rendering/global_illumination/restir/.
- Uninitialise (SERVERS): `release_all()`, `unregister_all()`, removes the `GiRestirProbe` singleton, deletes the
  probe and the backend.
- `servers/rendering/renderer_rd/environment/rtgi.cpp` treats a registered backend whose `is_active()` is false as
  absent: it calls `release(buffers)` on it and renders the frame without the module. It null-checks the backend
  everywhere, so a build without the module runs the plain RTGI path.
- An active backend may refuse a frame in `begin_frame`; `rtgi.cpp` then runs the plain RTGI path. RTGI counts every
  frame whose output does not feed its denoiser (refusal, or `gi/denoise` off) in its backend `refusals`.
- `begin_frame` refusals, their check order, the cleared reason and the status keys `refuses`, `refusals` and
  `refusal_reason` are under "Backend gates" (`modules/gi_restir/restir_gi.cpp:353-399`, `:935-938`).

## Settings (`modules/gi_restir/restir_settings.cpp`)

`modules/gi_restir/restir_settings.cpp` declares the table below in order. Each name is a project setting under
rendering/global_illumination/restir/; the range is the `PROPERTY_HINT_RANGE` hint. Settings other than
`gi/enabled` have no effect while it is off.

`gi/enabled` is the only setting read with `GLOBAL_GET_CACHED` (`restir_settings.cpp:304`). Every other `gi/*`
setting, and `wants_all_tiles` and `wants_replay`, is read with a plain `GLOBAL_GET` each frame (`_gi_bool`,
`_gi_int`, `_gi_text` and `_gi_float`, `restir_gi.cpp:233-265`), so a change takes effect on the next frame with no
restart, except where these notes state a cut or a timing caveat. The backend also reads
`rendering/global_illumination/rtgi/seed`, `origin_scale` and `origin_floor` (`GLOBAL_GET`; the last two clamped to
non-negative) and, at a variant's first compile, `rendering/reflections/sky_reflections/texture_array_reflections`
(see "Lazy compilation").

| Name | Type | Default | Effect |
|---|---|---|---|
| gi/enabled | bool | false | Turns the backend on; off keeps plain RTGI. |
| gi/mode | enum B1,B2,B2k,U1 | B2 | Bias mode of the reuse. |
| gi/spatial_neighbors | int 0..8 | 3 | Neighbours per spatial pass. |
| gi/spatial_radius | int 1..128 | 24 | Reuse radius in trace pixels. |
| gi/spatial_passes | int 1..2 | 1 | Spatial passes per frame. |
| gi/neighbor_selection | enum offsets,compatibility | offsets | Offset draw or compatibility pick. |
| gi/compat_candidates | int 1..64 | 16 | Taps scored when compatibility is chosen. |
| gi/compat_gate | enum similar,none,binary | similar | Internal; compatibility gate. |
| gi/compat_early_stop | bool | true | Internal; stop after enough scoring taps. |
| gi/m_temporal_cap | int 1..256 | 20 | History count cap before temporal combine. |
| gi/m_cap_follows_rate | bool | true | Scales the history cap by the RTGI ray fraction and limits the total cap to it plus 1. |
| gi/m_max | int 1..512 | 32 | Total sample cap after spatial reuse. |
| gi/m_light | int 1..32 | 2 | Sample cap while a light change is absorbed. |
| gi/m_light_frames | int 0..64 | 4 | Frames m_light applies after a light epoch bump; 0 off. |
| gi/n_target | int 1..64 | 8 | Sample count at which output coverage reaches 1. |
| gi/visibility_reuse | bool | false | Visibility test per reused sample (extra rays). |
| gi/fresh_lo | bool | false | Re-shades the selected hit each frame (one shade per pixel). |
| gi/blocked_keeps_coverage | bool | true | Keeps coverage with zero radiance when the final test fails. |
| gi/confidence_ramp | bool | false | Ramps the history caps after a history cut. |
| gi/ramp_m_min | int 1..255 | 4 | Cap at a history cut. |
| gi/ramp_frames | int 1..4096 | 64 | Frames over which the cap grows linearly. |
| gi/revalidate | enum off,budget,share | off | Stored-sample re-validation form. |
| gi/revalidate_period | int 2..16 | 3 | One frame in this many re-validates (budget). |
| gi/revalidate_share | int 1..1024 | 8 | One pixel in this many is re-traced (share). |
| gi/revalidate_threshold | int 1..1000 percent | 50 | Luminance change that cuts the count. |
| gi/revalidate_cut_m | int 0..16 | 1 | Count after a cut; 0 resets. |
| gi/trace_all_tiles | bool | true | Asks RTGI to trace every tile. |
| gi/confidence | enum frames,frames_and_m | frames | Confidence source on the RTGI route. |
| gi/replay_seed | bool | false | Keeps and reuses each sample's shade seed. |
| gi/event_reshade | bool | false | Light-event re-shading and capping. |
| gi/reshade_budget | int 0..100 percent | 25 | Trace pixels re-shaded per frame at most. |
| gi/event_m_cap | int 0..256 | 0 | Count limit near a lamp's first fire; 0 restarts. |
| gi/jump_m_cap | int 0..256 | 0 | Count limit on a jump frame; 0 restarts. |
| gi/event_reach_scale | float 0..64 | 2.0 | Reach multiplier for a first fire. |
| gi/event_solid_angle | float 0..12.566371 sr | 0.05 | Solid-angle gate for a first fire. |
| gi/spatial_hold | enum off,uncapped | uncapped | Keeps first-fire spheres across passes. |
| gi/spatial_hold_frames | int 1..256 | 40 | Frames a first-fire sphere stays held. |
| gi/sky_reread | bool | false | Sky samples read the live sky each pass. |
| gi/fold_k | int 0..64 | 0 | Stored-age bound; 0 off. |
| gi/own_sampler | bool | false | Module initial sampler instead of RTGI candidates. |
| gi/deterministic | bool | false | Internal; fixed seeds and neighbour offsets. |
| gi/dump_path | string | "" | Internal; replay-input dump folder; empty off. |
| gi/denoise | enum off,r2 | r2 | Output route into the RTGI denoiser. |
| gi/denoise_history_cap | int 1..255 | 4 | RTGI temporal history cap while denoise is r2. |
| gi/plane_tolerance | float 0.001..0.5 | 0.02 | Reuse refused at this plane distance over depth or more. |
| gi/normal_angle | float 1..90 degrees | 25 | Reuse refused when the receiver normals differ by this or more. |
| gi/roughness_tolerance | float 0..1 | 0.35 | Reuse refused when the roughness differs by more. |
| gi/jacobian_max | float 1..64 | 8 | Reuse refused when the shift changes the solid angle by more, either way. |
| gi/min_distance | float 0..1 | 0.03 | Reuse refused when the sample is nearer than this to either receiver. |
| gi/min_cosine | float 0..1 | 0.05 | Reuse refused when a cosine at the sample point is below this. |
| gi/weight_clamp | float 1..1000 | 20 | Final-pass weight limit as a multiple of the 3x3 median (biased modes). |
| gi/sky_distance | float 10..100000 | 1000 | Distance given to a sky sample. |
| gi/dirty_margin | float 0..1 | 0.01 | Padding of each changed-scene box. |
| gi/radiance_clamp | float 1..65504 | 60000 | Most radiance per channel a sample, sky read or shaded hit carries. |
| gi/revalidate_growth | float 1..1000 | 10 | Most a re-validated radiance grows per update, as a factor of its floored luminance. |
| gi/revalidate_floor | float 0.001..10 | 0.1 | Luminance floor of the growth limit and the change test. |
| gi/compat_score_scale | float 0.001..10 | 0.12615662 | Position falloff length of a compatibility tap score, as a share of the view distance. |
| gi/compat_stop_score | float 0..1 | 0.5 | Internal; score above which a tap counts for the early stop. |
| gi/sky_epoch_low | float 0.01..1 | 0.8 | Sky epoch starts when the mean sky falls below this share of the epoch value. |
| gi/sky_epoch_high | float 1..100 | 1.25 | Sky epoch starts when the mean sky rises above this multiple of the epoch value. |
| gi/sky_blur_lod | float 0..8 | 2 | Mip level a sky sample reads from the radiance sky texture. |

- All description strings start with "Experimental."; they live only in `restir_settings.cpp`, reach the probe
  status, and `doc/classes/ProjectSettings.xml` has no restir entries. `gi/compat_gate`, `gi/compat_early_stop`,
  `gi/compat_stop_score`, `gi/deterministic` and `gi/dump_path` are registered internal (`_GLOBAL_DEF` `p_internal`), so the editor does not
  show them.
- `gi/mode`: B1 biased reuse; B2 biased reuse with per-reuse visibility (default); B2k as B2 but keeps the count of
  some failed samples at zero weight (see "B2k, keep M (gi/mode B2k)",
  `modules/gi_restir/shaders/restir_gi.glsl:208`); U1 unbiased (tests).
- `gi/neighbor_selection` offsets draws random disk offsets; compatibility scores `gi/compat_candidates` taps by
  closeness in position and normal and picks `gi/spatial_neighbors` of them by weighted sampling without replacement
  (`rs_select_neighbours` in `shaders/restir_gi.glsl`; see Combine modes, "Compatibility selection"). `gi/compat_gate`
  similar keeps the plane, normal and class test, none keeps only the class test, binary weights every similar tap
  equally. `gi/compat_early_stop` stops scoring once more than `gi/spatial_neighbors` taps score above `gi/compat_stop_score`.
- `gi/m_cap_follows_rate` (frame params in `modules/gi_restir/restir_gi.cpp`): while the RTGI ray fraction (the
  share of pixels that receive a candidate each frame) is below 1, `gi/m_temporal_cap` becomes
  max(1, ceil(cap * fraction)) and `gi/m_max` becomes min(`gi/m_max`, that cap + 1). Off keeps the caps as set.
- `gi/m_light`, `gi/m_light_frames`: see "Invalidation and R2 integration", Light epoch
  (`modules/gi_restir/restir_gi.cpp:526-532`).
- `gi/confidence_ramp` grows the reservoir M cap, and `gi/denoise_history_cap` when `gi/denoise` is on, from
  `gi/ramp_m_min` (at most the cap) to its set value over `gi/ramp_frames` frames since the last history cut.
- `gi/fresh_lo` is ignored (warns once) when neither `gi/own_sampler` nor the DDGI shading inputs with the ray scene
  are available.
- `gi/revalidate`: budget makes every `gi/revalidate_period`-th frame ignore the incoming candidates and re-trace and
  re-shade the stored sample of every pixel that had a candidate; share re-traces one in `gi/revalidate_share` pixels
  every frame. Without `gi/replay_seed` (`shaders/restir_gi.glsl`): where the ray lands on the same point the stored
  radiance is updated (its luminance is limited to `gi/revalidate_growth` times max(stored luminance,
  `gi/revalidate_floor`)); a luminance change relative to max(old, new, `gi/revalidate_floor`) above
  `gi/revalidate_threshold` cuts the count to `gi/revalidate_cut_m`; a lost sample resets the reservoir. With
  `gi/replay_seed` the re-hit path replaces this (see Replay, "VALIDATE in replay mode"): no growth limit and no
  change cut, and a miss or mismatch writes Lo = 0 but keeps M.
- `gi/trace_all_tiles` (default on): on asks RTGI to trace every tile; off needs RTGI's tile texture and the module
  keeps reservoirs only in traced tiles; see "Traced tiles only and M in PACK"
  (`modules/gi_restir/restir_events.cpp:263-276`).
- `gi/confidence`: frames leaves the packed candidate's pdf word at 0, so coverage follows the RTGI history count
  alone; frames_and_m writes each packed pixel's reservoir sample count there, with `gi/trace_all_tiles` on or off,
  and hands RTGI `gi/n_target` and its history cap (`gi/denoise_history_cap`, or the ramped cap under
  `gi/confidence_ramp`); the RTGI side is under "R2 side: module output, route coverage, tile memory".
- `gi/event_reshade` re-shades stored samples inside a changed lamp's reach with their stored seed, caps the history
  count of receivers near a lamp's first fire, and caps every receiver on a sun or moon jump or a frame with lost
  events. It needs `gi/replay_seed`, the ray scene and the DDGI shading inputs, and `gi/revalidate` must not be
  budget; otherwise the frame is refused (see "Events (gi/event_reshade, default off)"). With `gi/denoise` r2 the RTGI
  temporal pass skips its own restarts, so an event restarts once. Off keeps the light epoch's `gi/m_light` cap.
- `gi/reshade_budget`, `gi/event_m_cap`, `gi/jump_m_cap`, `gi/event_reach_scale` and `gi/event_solid_angle` act as
  described under "Events (gi/event_reshade, default off)"; `gi/spatial_hold` and `gi/spatial_hold_frames` under
  "SPATIAL hold (gi/spatial_hold, default uncapped)" (`modules/gi_restir/restir_events.cpp`).
- `gi/sky_reread`: sky samples read the live sky each pass; conditions, the sky epoch, status values and cuts are
  under "Sky re-read (gi/sky_reread, default off)" (`modules/gi_restir/restir_gi.cpp:448-450`).
- `gi/fold_k`: stored-age bound (no stored candidate older than K view frames; each pixel restarts on a staggered
  frame), 0 off; rounding, the effective bound and the cut on change are under "Fold, the stored-age bound (gi/fold_k,
  default 0)" (`modules/gi_restir/restir_fold.cpp:30-72`).
- The nine reuse limits (`gi/plane_tolerance` to `gi/dirty_margin`) and the eight shader tuning values
  (`gi/radiance_clamp` to `gi/sky_blur_lod`, next bullet) are read once a frame by `restir_reuse_read` and written
  into `RestirFrameParams` by `restir_reuse_apply` (`restir_settings.cpp`), which holds each to its hinted range;
  `gi/normal_angle` is stored as float(cos(angle)) in `cos_normal`. `restir_reuse_defaults` reads the same 17 from
  the table without `ProjectSettings`. At the defaults the 17 fields equal the `RestirFrameParams`
  member defaults bit for bit (`modules/gi_restir/tests/test_restir_settings.h`). `vis_fail_limit` (2) is not a
  setting: in a rendered frame the final pass sets state bit 0x100 with every failed visibility test and clears the
  state on a pass, so the temporal pass refuses on the bit whatever the count limit is; the limit only acts on a
  state injected by the harness (`vis_l0`).
- The eight shader tuning values were literals in the shaders. Each default is the old literal, so the picture at the
  defaults is unchanged. All eight are the last eight floats of `RestirFrame` (`RestirFrameUBO`, bytes 432 to 463),
  written by `_upload` on every pass. They are not in `RestirEvents`, because `run_frame` writes zeros there when
  `F_EVENTS` is clear and VALIDATE and SPATIAL run without events. `gi/radiance_clamp` (byte 432) is the
  `RS_LO_CLAMP` macro, read in TEMPORAL, FINAL, SOURCE, VALIDATE and SKY_EPOCH and in the sky twins of TEMPORAL and
  SPATIAL; its top is the largest half, because stored radiance is a half. `gi/revalidate_growth` (436) and
  `gi/revalidate_floor` (440) are read in VALIDATE only. `gi/compat_score_scale` (444) and `gi/compat_stop_score`
  (456) are read in the compatibility variants of SPATIAL only. `gi/sky_epoch_low` (448) and `gi/sky_epoch_high`
  (452) are read in SKY_EPOCH only. `gi/sky_blur_lod` (460) is read by `rs_sky_radiance`: FINAL, SOURCE, VALIDATE
  and the sky twins of TEMPORAL and SPATIAL. The CPU statement of the sky epoch rule (`restir_sky_epoch_step`)
  keeps the default thresholds `RESTIR_SKY_EPOCH_LOW` and `RESTIR_SKY_EPOCH_HIGH`, which are also the member
  defaults. The two literals left are the round-to-nearest 0.5 of the spatial offset (`rs_round_away` and the
  `floor(offset + 0.5)` of the offset path), which is rounding and not a tuning value, and the 60000 in the
  debug luminance of `RESTIR_F_PROBE_PIXELS`, which is a half-range guard.
- A choice setting (`PROPERTY_HINT_ENUM`) accepts any string. `RestirGiBackend::_check_choices`, called first in
  `begin_frame`, re-reads the seven choices when `ProjectSettings::get_version` has changed, prints one error naming
  the first value outside its list (`restir_settings_invalid_choice`) when that differs from the last one reported,
  and the status key `invalid_choice` holds it (empty when every choice is listed). The readers then treat an
  unlisted value as: `gi/mode` B2, `gi/neighbor_selection` offsets, `gi/compat_gate` similar, `gi/revalidate` off,
  `gi/confidence` frames, `gi/spatial_hold` off, `gi/denoise` off.
- Tests in `modules/gi_restir/tests/test_restir_settings.h` cover: the 17 defaults against the member defaults;
  each limit at its hinted bounds, at the middle and outside both ends; the angle to cosine conversion; a listed and
  an unlisted choice value, a range setting and an unknown name.
- `gi/denoise`: r2 packs the module radiance into the RTGI candidate stream so the RTGI temporal, variance and
  a-trous passes denoise it; off leaves the module output unused and RTGI runs on its own candidates.
- `gi/replay_seed` keeps each sample's shade seed and re-shades the sample with it; see "Replay (`gi/replay_seed`,
  default off)" (`servers/rendering/renderer_rd/environment/rtgi.cpp:542`).

## Core include shim (`modules/gi_restir/restir_shim.py`)

`modules/gi_restir/SCsub` loads `modules/gi_restir/restir_shim.py` by file path with `importlib`. When `"RD_GLSL"` is
in `env["BUILDERS"]` it calls `write_shim(core_dir, shader_dir, build_dir)` before adding sources, with `core_dir`
set to servers/rendering/renderer_rd/shaders/environment, and compiles the three module shaders with `RD_GLSL`.

- `CORE_INCLUDES` is `ray_scene_inc.glsl`, `rtgi_inc.glsl`, `ray_scene_hit_shade_inc.glsl`; `EXTRA_INCLUDES` is
  `ddgi_inc.glsl`.
- `_shim`: `shaders/restir_core_inc.glsl` (generated, git-ignored) holds one `#include` per `CORE_INCLUDES`
  file, each path computed with `os.path.relpath` from the module shader folder. A missing core include or an OS
  error raises and stops the build. The run also writes `shaders/restir_core_<name>.glsl` wrappers for all four
  files (`CORE_INCLUDES` and `EXTRA_INCLUDES`).
- Drives: `glsl_builders.py` resolves includes with `os.path.relpath` from the engine build folder, and `relpath`
  cannot cross drives on Windows. `check_same_drive` (drive letter compared case-blind) raises `ShimError` naming
  both folders; `SCsub` prints it and exits 255.
- `include_closure` walks nested `#include`s from `CORE_INCLUDES + EXTRA_INCLUDES`, skips `thirdparty/` includes,
  and raises when a core include is missing.
- Dependencies: `SCsub` makes the gen headers of `restir_shim_check.glsl`, `restir_gi.glsl` and
  `restir_fold_test.glsl` depend on the shim, `glsl_builders.py`, `restir_gi_inc.glsl`, `restir_fold_inc.glsl`,
  the per-file wrappers, and the closure files returned by `write_shim`.
- Placeholder shader: `shaders/restir_shim_check.glsl` includes `restir_core_inc.glsl` so the build proves the
  includes resolve. It is never instantiated at run time. `modules/gi_restir/restir_probe.cpp` includes its
  generated header and uses it in a `static_assert`, so the header is built whenever the module is.

### Shade include contract (`servers/rendering/renderer_rd/shaders/environment/ray_scene_hit_shade_inc.glsl`)

`servers/rendering/renderer_rd/shaders/environment/ray_scene_hit_shade_inc.glsl` defines `shade_hit`. It has no
`#include` of its own; the including file must supply, before the include:

- the four macros RAY_SCENE_SHADE_MAX_DISTANCE, RAY_SCENE_SHADE_BOUNCE_ENERGY, RAY_SCENE_SHADE_EMISSION_EXPOSURE and
  RAY_SCENE_SHADE_MAX_RADIANCE. `ddgi_trace.glsl` sets them to ddgi.max_distance, params.bounce_energy,
  params.emission_exposure and DDGI_MAX_RADIANCE;
- `ray_scene_inc.glsl`, with RAY_SCENE_SET defined and RAY_SCENE_REPLAY defined when hits are replayed:
  RaySceneHit, ray_scene_hit_material, ray_scene_hit_normal, ray_scene.camera_to_anchor and gi_hit_direct_light
  (a `#define` to ray_scene_direct_light at the end of `ray_scene_inc.glsl`);
- `ddgi_inc.glsl` (ddgi_sample_direction, DDGI_NO_VIEW_LIMIT) with DDGI_SET, DDGI_UBO_BINDING and DDGI_SAMPLING
  defined, and the probe, irradiance and distance textures and linear sampler bound. `shade_hit` calls
  `ddgi_sample_direction` with view limit `hit.t * 0.5` at run time when the bounce energy is above 0, but the
  include is needed at compile time whatever the value;
- under MODE_SURFEL_TRACE only: ddgi.levels[0].spacing, the globals origin_surfel_position and
  origin_surfel_normal, and surfel_inc.glsl and surfel_gather_inc.glsl (surfel_lookup,
  surfel_eval_irradiance, surfel_blend, SURFEL_INVALID).

`modules/gi_restir/shaders/restir_gi.glsl` defines the four macros to the rs_shade_* variables (the max-radiance
macro to RS_LO_CLAMP) and includes the shade file under RESTIR_SHADE. RESTIR_SHADE is defined for RESTIR_RAYS in
MODE_FINAL, MODE_SOURCE and MODE_VALIDATE, and RAY_SCENE_REPLAY is defined under the same condition. The ray scene
is in set 1 (RAY_SCENE_SET) under RESTIR_RAYS; under RESTIR_SHADE or RS_SKY_REREAD, DDGI is in set 2 with
DDGI_UBO_BINDING 0. The DDGI textures (bindings 3 to 6) are bound under RESTIR_SHADE; the sky
radiance and sampler (bindings 7 and 8) under RESTIR_SHADE or RS_SKY_REREAD, which adds MODE_TEMPORAL and
MODE_SPATIAL when RESTIR_RAYS and SKY_REREAD are set. MODE_SKY_EPOCH undefines RESTIR_RAYS. The module never defines
MODE_SURFEL_TRACE and needs neither the surfel includes nor the origin_surfel_* globals.

The import seed (gi/replay_seed) comes from rtgi_anchor_pixel and rtgi_shade_key in `rtgi_inc.glsl`, the helpers
`ddgi_trace.glsl`'s MODE_RTGI uses, so the imported tag is RTGI's seed at that pixel.

A module shader that shades hits defines the four macros, includes `restir_core_ray_scene_inc.glsl` and
`restir_core_ddgi_inc.glsl`, then `restir_core_ray_scene_hit_shade_inc.glsl`. `restir_core_inc.glsl` includes the
shade file with none of the macros defined; only `restir_shim_check.glsl` includes it, and that shader is never
instantiated at run time, so the build only proves that the paths resolve.

## Build cases

`modules/gi_restir/config.py` gates the module on `env["rendering_device"]`, and `SConstruct` adds the option
`module_gi_restir_enabled`, on by default when modules are enabled by default (the module defines no `is_enabled`).
The configurations that must build:

- module absent: the engine builds with no module files; `rtgi.cpp` null-checks the backend.
- module present with `module_gi_restir_enabled=no`: module sources are skipped.
- module present with the option at its default: built in.
- `custom_modules` naming a copy of the module outside the tree, with identical sources: the module shader folder
  and the engine build folder must be on the same drive.
- D3D12: the module has no D3D12-specific source. `RestirGiBackend::begin_frame` chooses `VARIANT_HW` or
  `VARIANT_SW` at run time from `RayScene::uses_hardware()` (`VARIANT_NONE` without a ready ray scene).

The module folder holds no third-party code and no per-platform source.

## Probe

`modules/gi_restir/restir_probe.cpp` defines `RestirProbe`, registered with ClassDB and exposed as the Engine
singleton `GiRestirProbe`. `get_status()` returns a Dictionary:

- `module`: always "gi_restir".
- `rtgi_registered`: whether a backend is registered; `rtgi_name` is its `get_name()` ("restir_gi") or empty.
- `rtgi_active`: the backend's `is_active()`, false when no backend is registered.
- `rtgi_status`: the backend's `get_status()` dictionary, present only when a backend is registered.
- `settings`: each full setting name mapped to a Dictionary with `value`, `description` and `internal`, in
  declaration order (`restir_settings_snapshot`).

The probe also binds harness entry points (`configure`, `run_case`, `read_case_file`, `run_fold_test`,
`mirror_fold_test`, record, pixel-sum, capture and fold accessors) that run through `RestirHarness` and
`RestirGiCore`.

## Backend contracts

`servers/rendering/renderer_rd/environment/gi_module_hooks.h` defines `RendererRD::RtgiResampleBackend`;
`gi_module_hooks.cpp` holds the singleton registry (one backend pointer). `rtgi.cpp` calls the backend on the render
thread.

- `is_active()` is read every frame before tracing. The module's version only reads a cached setting. A false
  result makes core call `release(buffers)` and treat the backend as absent.
- `wants_all_tiles()` (default true) is the hook for the tile skip (see "Traced tiles only and M in PACK");
  `wants_replay()` (default false) is the hook for FRAME_FLAG_REPLAY (see "Replay").
- `begin_frame()` returns false to refuse the frame; core then runs the plain RTGI path. Its input
  `RtgiFrameInputs.history_cut` is true on the first backend frame of the buffers and on a history or camera cut.
  `resample()` must fill `RtgiResampleOutput`. The denoiser consumes the output only when `valid`, `denoise` and
  both `radiance` and `coverage` are valid RIDs. Then `history_cap` above 0 replaces RTGI's temporal history cap
  (clamped 1 to 255), `history_cut` records a module cut
  (`CUT_MODULE`, only when RTGI's own cut for the frame is none),
  and `mode` RTGI_OUTPUT_MODULE sets
  FRAME_FLAG_MODULE_OUTPUT, with `n_target` above 0 (clamped to 255) as the route's coverage target.
- `release(buffers)` is called with the render buffers from three places: `RtgiPass::release`
  (`render_forward_clustered.cpp`, on a frame where RTGI does not run), `RtgiBuffers::free_data` (reached from
  `RenderSceneBuffersRD::cleanup`), and `rtgi.cpp` when `is_active()` is false.
  `release(nullptr)` is called only by `release_all()` in the module's SERVERS
  uninitialise; `unregister_all()` only clears the registered pointer. The module's `release(nullptr)` frees every
  state and the core, so repeating it is safe; a backend must not touch the device after it.
- `get_status()` fills the dictionary the probe publishes and RTGI publishes under its backend entry; `get_name()`
  returns "restir_gi".

## Provenance

The module's host contract is `servers/rendering/renderer_rd/environment/gi_module_hooks.h` and
`gi_module_hooks.cpp`; module discovery and the `module_gi_restir_enabled` option come from `SConstruct`;
include resolution comes from `glsl_builders.py`; settings use `core/config/project_settings.h`.

## ReSTIR GI core (gi backend "restir_gi")

`modules/gi_restir/restir_gi_core.h` and `restir_gi_core.cpp` hold resources, pipelines and pass dispatch;
`modules/gi_restir/restir_gi.h` and `restir_gi.cpp` hold the backend (frame gates, history cuts, epoch handling,
status); `restir_test.h` defines the probe options and per-frame records; `restir_harness.cpp` the replay harness;
`restir_probe.h/.cpp` the probe object. `shaders/restir_gi_inc.glsl` and `shaders/restir_gi.glsl` hold the compute
modes CLEAR, TEMPORAL, SPATIAL, FINAL, SOURCE, CAPTURE, PACK, VALIDATE and SKY_EPOCH. SPATIAL has compatibility and
wide twins (see "Pass chain"); TEMPORAL and SPATIAL (with its twins) have sky-reread twins. The SW and HW variants are
selected by the ray scene.

### Storage and frame buffers (`modules/gi_restir/restir_gi_core.cpp`)

A reservoir is 48 bytes per pixel in a 3-layer RGBA32UI 2D array (`rs_load` and `rs_store` in
`shaders/restir_gi_inc.glsl`). Layer 0: xs.xyz as float bits, packed normal in .w. Layer 1: Lo.rg in .x and Lo.b in
the low 16 bits of .y as halfs, flags in the high 16 bits of .y, W as a float in .z, M in the low 16 bits and age in
the high 16 bits of .w. Layer 2: receiver depth, packed receiver normal, epoch tag, sample tag. Three reservoir arrays
are allocated: the temporal pass ping-pongs two and the third holds a spatial result when feedback is off.
`RestirFrameUBO` is 464 bytes and `RestirDirtyUBO` 2048 bytes, both enforced by `static_assert`
(`restir_gi_core.cpp:113-114`).

Per view, `RestirGiResources` (`restir_gi_core.h`, built by `RestirGiResources::allocate`, which first frees
everything) holds:

- three reservoir arrays (RGBA32UI, 3 layers; "reservoir set 0" to "reservoir set 2");
- `vis_state` (R32_UINT; the low byte is the failed-visibility count and 0x100 means the last visibility ray failed),
  `info` (R32_UINT), `radiance` (RGBA16F) and `coverage` (R16F);
- `debug` (RGBA16F), created up front when readback or a debug view is on and otherwise by `ensure_debug` on demand;
  `debug_dummy` and `sel_dummy`, 1x1 placeholders (`sel_image` is under "Statistics");
- `stats` (16 floats per 8x8 group), `pass_stats` (4 rows of 32 floats per group), `event_bins` (256 uints),
  `event_ubo` (2368 bytes) and `dirty_ubo` (2048 bytes);
- seven `RestirFrameUBO` (`ubo[0]` to `ubo[5]` and `count_ubo`): `ubo[0]` for TEMPORAL, VALIDATE and SKY_EPOCH
  (`pass_index` 0), `ubo[1]` and `ubo[2]` for SPATIAL passes 1 and 2, `ubo[3]` for FINAL and PACK (`pass_index` 3),
  `ubo[4]` for SOURCE, `ubo[5]` for CLEAR, and `count_ubo` for the VALIDATE count dispatch (`pass_index` 4).

Each is logged in `alloc_log` with expected and measured bytes (the uniform buffers are one entry, "uniform buffers").
Source: `modules/gi_restir/restir_gi_core.cpp:188-217`.

Reservoir flags (the high half of layer 1 `.y`; `shaders/restir_gi_inc.glsl:1-10`): `RF_SKY` 1, `RF_VALID` 2,
`RF_SPATIAL` 8, `RF_BACKFACE` 32, `RF_EMISSIVE` 64, `RF_TEMPORAL` 128, bits 8 to 13 the Fold `s` and bit 14
`RF_MV` (see Fold). Bits 4 and 16 of the reservoir flags are unused. `rs_finish` sets `RF_TEMPORAL` or
`RF_SPATIAL` when the selected technique is not the canonical and adds 1 to the age only for the temporal pass
(`restir_gi.glsl:280-289`). The frame flag bits 64 and 256 have no GLSL name and no C++ `RESTIR_F_*` entry.

Set 0 bindings (declared in `shaders/restir_gi_inc.glsl:138-431`, each under its own MODE_* guard; the host side is
`run_frame`, `run_source`, `run_capture`, `run_pack` and `clear_history` in `restir_gi_core.cpp`, which add the
entries after binding 0 in `_dispatch`, `restir_gi_core.cpp:677-702`). A shader declares only the bindings of its own
mode. Passes: CLEAR, TEMPORAL (plain, sky twin, Fold dump and lineage forms), SPATIAL (four forms and their sky
twins), FINAL, SOURCE, CAPTURE, PACK, VALIDATE and SKY_EPOCH.

- 0 `RestirFrame` UBO (`rf`), every pass. The host binds `ubo[0]` to TEMPORAL, VALIDATE and SKY_EPOCH, `ubo[1]` and
  `ubo[2]` to SPATIAL, `ubo[3]` to FINAL and PACK, `ubo[4]` to SOURCE, `ubo[5]` to CLEAR, `count_ubo` to the extra
  VALIDATE count dispatch and a one-off UBO to CAPTURE.
- 1 `geo_pos_cur` and 2 `geo_nrm_cur` (texture2D): TEMPORAL, SPATIAL, FINAL, SOURCE and CAPTURE. PACK declares only 1.
- 3 `geo_pos_prev` and 4 `geo_nrm_prev` (texture2D): TEMPORAL, CAPTURE and VALIDATE.
- 5 `cand_first` and 6 `cand_second` (utexture2D): TEMPORAL and CAPTURE read both. FINAL and VALIDATE declare both but
  their code reads only 6; both still bind 5, since a set must cover every binding its pass declares.
- 7 `res_in` (utexture2DArray): TEMPORAL (`reservoir[hist]`), SPATIAL (the read set), FINAL and PACK
  (`reservoir[final_index]`). CLEAR, SOURCE, CAPTURE and VALIDATE have no 7.
- 8 `res_out` (rgba32ui, writeonly): CLEAR (all three sets in turn), TEMPORAL (`reservoir[hist ^ 1]`) and SPATIAL (the
  write set). VALIDATE declares it as read-write `res_io` on `reservoir[hist]`. FINAL, PACK, SOURCE and CAPTURE
  have no 8.
- 9 `vis_state` (r32ui): CLEAR, TEMPORAL and FINAL. 10 `info_image` (r32ui): CLEAR, TEMPORAL, SPATIAL, FINAL and PACK.
- 11 and 12: FINAL writes `out_radiance` (rgba16f) and `out_coverage` (r16f) as storage images; PACK reads the same
  two resources as textures (`pk_radiance`, `pk_coverage`). 13 `out_debug` (rgba16f) and 14 `stats` (storage buffer)
  are FINAL only. The host binds `debug_dummy` at 13 when `debug` is not allocated.
- 15 `src_first` and 16 `src_second` (rgba32ui, writeonly): SOURCE, onto RTGI's two candidate textures. PACK
  declares 16 as `pk_second` (writeonly, the second candidate texture) and no 15.
- 17 to 22 `cap_pos`, `cap_nrm`, `cap_ppos`, `cap_pnrm` (rgba32f), `cap_first` and `cap_second` (rgba32ui): CAPTURE.
- 23 `pass_stats` (writeonly storage buffer): TEMPORAL, SPATIAL and VALIDATE. 24 `RestirDirty` UBO: TEMPORAL only.
- 25 `sel_image` (rg32ui, writeonly array): the compatibility SPATIAL forms (`PASS_SPATIAL_COMPAT`) only; the host
  binds `sel_dummy` there unless `F_PROBE_STATS` is set (`restir_gi_core.cpp:984-986`).
- 26 `RestirEvents` UBO: TEMPORAL, SPATIAL, VALIDATE and SKY_EPOCH. 27 `rs_tiles` (utexture2D): TEMPORAL, SPATIAL,
  FINAL and PACK; with no tile texture the host binds `dummy_tiles` (1x1 R32_UINT, `RTGI_TILE_TRACE` set).
- 28 `RestirEventBins` (storage buffer): VALIDATE only.
- 29 `RestirSkyEpoch` (storage buffer): SKY_EPOCH read-write; the TEMPORAL sky twin reads it (readonly) and the host
  binds it only when sky reread is on. 30 `epoch_sky` (texture2D or texture2DArray under USE_RADIANCE_OCTMAP_ARRAY)
  and 31 `epoch_sampler`: SKY_EPOCH only (the white default texture when no sky texture is given).
- 32 `RestirFoldDump` (storage buffer): the TEMPORAL Fold dump and lineage forms. 33 `rs_fold_lineage_in` (rg32ui,
  readonly), 34 `rs_fold_lineage_out` (rg32ui, writeonly) and 35 `rs_fold_refused_id` (r32ui, writeonly): the lineage
  form only.
- Set 1 is the ray scene (`RayScene::append_uniforms`). `_dispatch` binds it only for a ray variant and never for
  SKY_EPOCH; CLEAR, CAPTURE and PACK always run on VARIANT_NONE, so it reaches TEMPORAL, SPATIAL, FINAL, SOURCE
  and VALIDATE.
- Set 2 is the shade set (`_shade_set`, `restir_gi_core.cpp:653-675`), bound for FINAL, SOURCE, VALIDATE and the sky
  twins of TEMPORAL and SPATIAL. FINAL, SOURCE and VALIDATE use the DDGI UBO at 0, probe 3, irradiance 4, distance 5,
  linear sampler 6, sky radiance 7 and its sampler 8; the sky twins declare only 0, 7 and 8. Without all four DDGI
  RIDs the host binds a 512-byte zero UBO (`dummy_ubo`) and 1x1 `dummy_probe` and `dummy_float` textures.

### Pass chain (`modules/gi_restir/restir_gi_core.cpp`, `RestirGiCore::run_frame`)

Order: optional VALIDATE (re-validation or event validation; needs a ray scene and history in use; event validation
first runs one extra VALIDATE dispatch with `pass_index` 4 after clearing `event_bins`), optional SKY_EPOCH (sky
reread), TEMPORAL, 0 to 2 SPATIAL passes, FINAL. With sky reread active the TEMPORAL and SPATIAL dispatches use their
sky-reread twins. `RESTIR_F_VIS` (value 32, bit 5) is set only when a ray-scene variant is in use; without it no
visibility test runs anywhere. TEMPORAL reads the set at `history_index` and writes the other set. Each spatial pass
reads the previous pass's output. With `spatial_feedback` on, the spatial passes ping-pong the history sets (temporal
output set -> history set, then back); with it off, the first spatial pass writes the third set and the second reads
it and writes the previous history set. FINAL reads the last spatial output, or the temporal output when there is no
spatial pass. `history_index` becomes the final output's set when feedback is on, otherwise the temporal output's set;
no copy occurs. `spatial_feedback` defaults on in U1 and off in B1, B2 and B2k, and a probe option can override it
(`restir_gi.cpp:453`). The spatial pass count is `spatial_passes` clamped to 1..2, or 0 when `gi/spatial_neighbors` is
0 (range 0..8, default 3), so no spatial pass runs and spatial_combines is 0. CLEAR runs when a state is allocated or
reallocated (`RestirGiCore::clear_history`).

SPATIAL is compiled in plain and wide forms. `run_frame` picks the wide form (`SPATIAL_WIDE`, `MAX_CAND` 9) when
`gi/spatial_neighbors` is above `RESTIR_SPATIAL_SMALL_COUNT` (3) and the plain form (`MAX_CAND` 4) otherwise; TEMPORAL
has `MAX_CAND` 2. A pass draws min(`gi/spatial_neighbors`, `MAX_CAND` - 1) neighbours, so plain serves 0 to 3 and wide
serves 4 to 8. Compatibility selection caps its picks at `COMPAT_MAX_M` (8). Plain, compatibility, wide and
compatibility-wide are four pipelines, each with a sky twin (`restir_gi_core.cpp:987-988`, `restir_gi_core.h:84`,
`shaders/restir_gi_inc.glsl:66-72`, `shaders/restir_gi.glsl:893`).

Fold-dump variant: when `RestirGiResources::fold_dump` is valid, `run_frame` replaces the TEMPORAL dispatch with a
dump or lineage build; see "Fold dump and self-test" (`modules/gi_restir/restir_gi_core.cpp:938-966`).

Shade set (`RestirGiCore::_shade_set`, `restir_gi_core.cpp:653-675`): the real DDGI UBO, probe, irradiance and
distance textures are bound only when all four RIDs are valid; otherwise a 512-byte zero UBO and 1x1 placeholders
(`dummy_probe` RGBA32UI, `dummy_float` RGBA16F) are bound. The sky binding (set 2, binding 7) takes the sky texture
from the inputs and falls back to the white default whenever none is given, whatever the DDGI state.
`run_frame` also clears `F_REAL_SKY` and `F_SHADE_DDGI` when no DDGI UBO is given (`restir_gi_core.cpp:856-858`).

GPU timestamp names (`marker_names`, `restir_gi_core.cpp:686-687`): "ReSTIR Clear", "ReSTIR Temporal" (also the sky
twin), "ReSTIR Spatial" (all eight spatial pipelines), "ReSTIR Final", "ReSTIR Source", "ReSTIR Capture", "ReSTIR
Pack", "ReSTIR Validate" and "ReSTIR Sky Epoch"; the dump and lineage variants use their own names (see "Fold dump and
self-test" and "Fold lineage").

### Receiver origin (`modules/gi_restir/shaders/restir_gi_inc.glsl`)

`rs_origin_offset_vector` places every ray origin at pos + n * offset. The offset is the larger of the depth-derived
position error scaled by `origin_scale` (default 2) and the floor `origin_floor * |view position|` (default 0):
error = 2^-23 * (4 * |view position| + 4 * max |anchor position component|) + footprint / 256.
`footprint` is the larger of two per-axis neighbour offset lengths; on each axis the tap with the smaller depth gap
along the view direction is chosen among the geometry texture's left/right and up/down neighbours (`rs_pick_gap`),
and a tap with validity w <= 0.5 is skipped. Tap positions are jitter-corrected (`rs_true_view`) with the jitter of
that receiver's frame. n is the geometric normal from those two offsets (cross product, flipped to face the camera)
when both axes have a neighbour and its cosine with the shading normal exceeds 0.7; otherwise n is the shading normal.
Directions, cosines, Jacobians and visibility rays use this origin.

`origin_scale` and `origin_floor` are the RTGI settings `rendering/global_illumination/rtgi/origin_scale` and
`.../origin_floor` (internal, defaults 2.0 and 0.0, `servers/rendering/rendering_server.cpp:3929-3930`), both
clamped non-negative (`restir_gi.cpp:669-670`). Core's trace (`rtgi_geometric_normal` and
`rtgi_origin_offset` in `servers/rendering/renderer_rd/shaders/environment/ddgi_trace.glsl`) uses the same rule with
the screen-resolution depth buffer (a tap with depth 0 is skipped; the footprint falls back to the right-neighbour
distance when an axis has no tap); the module samples the geometry textures at trace resolution, so the footprint
term differs.

### Fresh candidates (`modules/gi_restir/shaders/restir_gi.glsl`, `rs_from_candidate`)

TEMPORAL builds the canonical technique from the RTGI candidate record: W = 1 / pdf, with pdf the float in the
record's second word .z, M = 1 and age 0; Lo is clamped to [0, `RS_LO_CLAMP`] (`gi/radiance_clamp`, default 60000). An empty reservoir (M 0)
comes from a record flagged no-candidate and from a re-validation frame. A candidate whose Lo is not finite counts
`lo_refused`, and one whose pdf is not finite and positive counts `pdf_refused`; both give M = 1 with the valid flag
clear, so the canonical is not on. A canonical that is not on contributes no M in B1, B2 and B2k, and contributes its
M in U1; `no_canonical` counts temporal pixels whose canonical is not on.

### Combine modes (`modules/gi_restir/shaders/restir_gi.glsl`, `modules/gi_restir/shaders/restir_gi_inc.glsl`)

`gi/mode` selects B1, B2 (default), B2k or U1 (`RESTIR_MODE_B1` 0, `_B2` 1, `_U1` 2, `_B2K` 3). The flags map U1 to
`RESTIR_F_UNBIASED`, B2 and B2k to `RESTIR_F_VIS_REUSE`, and B2k to `RESTIR_F_KEEP_M`; `gi/visibility_reuse` (default
off) also sets `RESTIR_F_VIS_REUSE`, which is how B1 gets the reuse test. The reuse test applies only when
`RESTIR_F_VIS` is set. The temporal history M entering a combine is min(M, `gi/m_temporal_cap`) (default 20,
`restir_gi.glsl:598`); a spatial neighbour's M is min(M, `m_max`) (`restir_gi.glsl:934`). The backend lowers the caps
in `modules/gi_restir/restir_gi.cpp:514-532`: see Settings (`gi/m_cap_follows_rate`, `gi/confidence_ramp`) and Light
epoch.

- B1: biased MIS. Each technique's MIS weight is its M over the sum of M of the techniques that count. A technique
  whose sample fails the shift test or the history state adds neither weight nor M.
- B2: B1 plus a receiver-side visibility test on every reused sample. A neighbour that shifts validly but is blocked
  at the receiver has `on` cleared, so it adds neither weight nor M.
- U1: pairwise MIS with visibility tested on both sides. The canonical share is the mean over present neighbours of
  a_c0 / (a_c0 + a_at_c); a neighbour's share is a_self / (a_self + M_c * phat), divided by the neighbour count.
- B2k: B2 plus `RESTIR_F_KEEP_M`; what is kept is under "B2k, keep M (gi/mode B2k)"
  (`modules/gi_restir/shaders/restir_gi.glsl:208`).

Selection weight is mis * phat_r * W * Jacobian; the result's W is the weight sum over the selected phat_r. In U1 a
neighbour whose sample fails the shift test (distance, cosine, |J| range) stays in the technique set with weight 0;
removing it would change the canonical share per sample.

**Similarity and shift limits** (`shaders/restir_gi.glsl:259-277`, `shaders/restir_gi_inc.glsl:616-641`,
`restir_gi_core.h:121,134-143`). `rs_similar` passes a neighbour when it is valid, `|dot(n_r, p_n - p_r)| / depth` is
below `tol_plane` (0.02), `dot(n_r, n_n)` is above `cos_normal` (0.9063078, 25 degrees) and `|rough_r - rough_n|` is
at most `class_tol` (0.35). The class is the roughness, read from the geometry normal texture's `.w`
(`Receiver.rough`). `rs_shift_jacobian` (a sky sample returns 1) rejects with `REJ_DISTANCE` when either distance from
the stored point to the source receiver q or to the receiver r is below `d_min` (0.03), and with `REJ_JACOBIAN` when
either cosine at the stored point is below `cos_min` (0.05) or when `jac = (cr / cq) * lq^2 / lr^2` falls outside [1 /
`jac_max`, `jac_max`] (`jac_max` 8); `cq` and `cr` are the cosines between the stored normal and the directions to q
and r, and `lq` and `lr` the distances. The numbers in brackets are the defaults of `gi/plane_tolerance`,
`gi/normal_angle`, `gi/roughness_tolerance`, `gi/min_distance`, `gi/min_cosine` and `gi/jacobian_max`; `w_clamp` (20),
`sky_distance` (1000) and `dirty_margin` (0.01) come from `gi/weight_clamp`, `gi/sky_distance` and `gi/dirty_margin`
(see Settings, the nine reuse limits), and `vis_fail_limit` keeps its member default 2. The probe options `d_min`,
`cos_min`, `jac_max`, `vis_fail_limit` and `w_clamp` are applied after the settings and override them.

**Compatibility selection** (`rs_select_neighbours`, `shaders/restir_gi.glsl:705-849`) is weighted sampling without
replacement, not a probability table. Per pixel, a start index from `rs_rand(COMPAT_START_DIM + pass_index)`
(`COMPAT_START_DIM` 160) feeds up to min(`gi/compat_candidates`, `COMPAT_MAX_K` 64) taps; tap t is the concentric-disk
offset of radius `spatial_radius` at index start + t (`rs_compat_offset`), and out-of-frame, untraced, duplicate and
zero-offset taps are skipped. The score is `h = exp(-|true_view_r - true_view_q| / (|true_view_r| * s)) * clamp(dot(n_r, n_q), 0, 1)^8`, with
s = `gi/compat_score_scale` (default 0.12615662), flushed to 0 below `FLT_MIN`. The tap weight w is h when `gi/compat_gate` is similar
and the tap passes `rs_similar`, 1 for binary on a similar tap, and h for none, which tests only the class. Each tap
with w above 0 gets the key `log(u) / w`, u from `rs_rand(COMPAT_KEY_DIM + t)` (`COMPAT_KEY_DIM` 96), and the top m =
clamp(wanted, 1, `COMPAT_MAX_M`) keys are kept, where wanted = min(`gi/spatial_neighbors`, `MAX_CAND` - 1). The early
stop counts taps with h above `gi/compat_stop_score` (default 0.5) that were inserted or rejected by key and stops
when the count exceeds m.

### Stored M (`modules/gi_restir/shaders/restir_gi_inc.glsl`, `rs_combine`; `restir_gi.glsl`, `rs_finish`)

In U1, stored M = min(canonical M + M of every present technique, m_max). It does not depend on the stored sample. A
technique whose sample fails the shift keeps weight 0 and keeps its M. In B1 and B2, the weight denominator and stored
M are the sum of M over the techniques that contribute (the canonical when on, plus neighbours that shift validly and
pass the mode's visibility test), capped at m_max; a present technique whose sample fails adds neither M nor weight.
B2k also counts the techniques kept by `RESTIR_F_KEEP_M`. The M returned by `rs_combine` is the same total that
normalises the weights. The stored age grows by 1 (cap 65535) only when the temporal neighbour is selected.

A technique is present when `rs_fill_neighbour` runs (`restir_gi.glsl:183`).

- Temporal (`rs_temporal_pixel`, `restir_gi.glsl:532`): needs history in use (`RESTIR_F_HISTORY`), a previous position
  inside the frame, and passing the similar test (`rs_similar`, `restir_gi.glsl:259`: previous receiver valid, plane,
  normal, class), then M > 0 and an equal epoch tag. A history sample whose path hits a dirty box is handled per mode
  as under "Dirty test" (`modules/gi_restir/shaders/restir_gi.glsl:593-611`).
- Spatial, offset selection (`rs_spatial_pixel`, `restir_gi.glsl:862`): the neighbour must be inside the frame and
  not the pixel itself, traced, similar and have M > 0.
- Spatial, compatibility selection (`rs_select_neighbours`, `restir_gi.glsl:764`): tap scoring skips out-of-frame,
  untraced, duplicate and zero-offset taps. The gate sets the tap weight: `similar` keeps taps that pass the similar
  test with the score as weight, `binary` the same with weight 1, `none` tests only the class. The neighbour then
  needs M > 0.

A present technique stays at weight 0 when, in the biased modes, the `vis_fail_limit` or a held 0x100 vis state
applies, when a history reservoir has M > 0 but an invalid sample, or when a spatial neighbour has M > 0 but an
invalid sample. A reservoir with M = 0 is absent.

### Final output and coverage (`modules/gi_restir/shaders/restir_gi.glsl`, `MODE_FINAL`)

Radiance is lo * cos / pi * W. Coverage is clamp(M / n_target, 0, 1) for every pixel whose reservoir is held: M > 0,
the valid flag set and W finite (`n_target` is `gi/n_target`, default 8). A valid sample with zero radiance still
gets coverage; coverage is not keyed on W = 0. Coverage is 0 for untraced pixels, a reservoir that is not held, or a
failed final visibility ray unless `gi/blocked_keeps_coverage` (default on) sets `RESTIR_F_KEEP_BLOCKED`. In the
biased modes W is clamped to w_clamp * median (`gi/weight_clamp`, default 20) when the 3x3 neighbourhood holds at
least 3 finite positive W values with M > 0; U1 skips the clamp.

**Readback counts** (`modules/gi_restir/restir_gi.cpp`). The backend publishes no provider record:
`RtgiResampleOutput` carries no record and no debug texture. With readback, `geometry` counts the pixels of traced tiles
whose receiver is valid (geometry texture `.w` above 0.5: no sky, no untraced tile), whether or not they hold a
sample or are ray-thinned; `m_sum` adds the reservoir M of those pixels (0 for an empty one); `output_pixels` counts
those with coverage above 0, which needs a held sample and, if its final visibility ray failed,
`gi/blocked_keeps_coverage` (`shaders/restir_gi.glsl:1138-1254`, `restir_gi_core.cpp:1042-1051`).

### Own sampler (`modules/gi_restir/shaders/restir_gi.glsl`, `MODE_SOURCE`)

`gi/own_sampler` (default off) runs SOURCE before the frame (except on a re-validation frame) and writes the module's
candidates into the RTGI candidate textures, replacing core's. Each valid receiver gets one cosine-weighted ray from
its origin (pdf = max(cos, 1e-4) / pi). On a hit without `gi/replay_seed`, Lo = emission * emission_exposure + albedo
* gi_hit_direct_light (no DDGI bounce), clamped to `RS_LO_CLAMP`, and a backface hit on a one-sided material gives Lo
0; with `gi/replay_seed` the hit goes through `shade_hit` with the DDGI bounce energy 0 (`rs_shade_setup`). On a miss
the sky radiance (`rs_sky_radiance`, inputs under "Sky re-read (gi/sky_reread, default off)") is used: the probe's
constant `sky` colour when `constant_sky` is set or no DDGI shading inputs exist, otherwise the DDGI sky. With
`constant_sky` the emission exposure stays at its default 1. An invalid receiver writes no candidate. The frame is
refused when the sampler is on and no ready ray scene exists.

SOURCE draws its direction from `rtgi_rand2(px, frame, RTGI_RNG_DIRECTION, rf.seed)`, with `rf.seed` = `rtgi/seed` +
`seed_offset`. Without `gi/replay_seed` the candidate tag is `(px.y * trace_w + px.x) ^ frame` and the trace seed is
`ray_scene_hash(px.y * 65537 + px.x + frame * 6271 + 0x510e527f)`; with replay the tag and the trace seed are both
`ray_scene_hash(px.y * 65537 + px.x + frame * 6271)`. A receiver that is not valid writes a `NO_CANDIDATE |
NOT_TRACED` word carrying the tag (`shaders/restir_gi.glsl:1288-1356`).

### Statistics (`modules/gi_restir/shaders/restir_gi.glsl`, `modules/gi_restir/restir_gi_core.cpp`)

The final pass writes 16 floats (`STATS_STRIDE`) per 8x8 group through shared-memory serial sums (no float atomics);
`read_stats` sums the groups into double fields. TEMPORAL, SPATIAL and VALIDATE write `PS_STRIDE` = 32 floats per 8x8
group into a 4-row pass-stats buffer (temporal, spatial 1, spatial 2, validate), cleared every frame. TEMPORAL and
SPATIAL write row `pass_index` (0, 1, 2) through `rs_stats_store(row)`; VALIDATE always writes the fixed row
`PS_ROW_VALIDATE` = 3, whatever its `pass_index`, and the `pass_index` 4 count dispatch writes no row
(`restir_gi.glsl:681,979,1716`). Entries include tested techniques (a valid sample reaching the shift test), rejects
by d_min, cos_min and |J| range (by kind), the energy of the tested candidates plus the canonical (M * p-hat * W *
Jacobian), the energy of the rejected ones, pdf refusals, techniques present, present techniques whose sample is
unusable (kept at weight 0), spatial combines (pixels that combined at least one neighbour), lo refusals and pixels
with no canonical. `jac_raw` (`rs_jacobian_raw`, `restir_gi_inc.glsl:646`) is the Jacobian without the range test (1
for sky samples), clamped to [0, 1000], with both distances floored at 1e-4 and the cosine at the sample's source
receiver floored at 1e-3. Energy sums skip NaN and Inf. Pass stats are written only when `RESTIR_F_PROBE_STATS` (value
128, bit 7) is set, which the probe option `readback` sets (`run_case` also sets it for compatibility selection and
`fold_dump`).

Pass-stats slots 0 to 15 are, in order, `PS_TESTED`, `PS_REJ_DIST`, `PS_REJ_COS`, `PS_REJ_JAC`, `PS_ENERGY_ALL`,
`PS_ENERGY_TRUNC`, `PS_PDF_REFUSED`, `PS_PRESENT`, `PS_ZERO_WEIGHT`, `PS_SPATIAL_COMBINES`, `PS_LO_REFUSED`,
`PS_NO_CANON`, `PS_COMPAT_TAPS`, `PS_COMPAT_STOPS`, `PS_COMPAT_EMPTY` and `PS_COMPAT_HSUM`
(`shaders/restir_gi_inc.glsl:77-92`); the slots from 16 up are named where they are used. `read_stats` adds the
compatibility slots 12 to 15 over the temporal and both spatial rows into `compat_taps`, `compat_early_stops`,
`compat_empty` and `compat_h_sum`, which the status `stats` dictionary, `get_records` (columns 31 to 34) and the
`run_case` stats carry (`restir_gi_core.cpp:1078-1081`).

With `gi/neighbor_selection` compatibility and `F_PROBE_STATS`, `run_frame` creates `sel_image` (R32G32_UINT, 2 layers
for spatial passes 1 and 2, binding 25); with compatibility on and no `F_PROBE_STATS`, a 1x1 `sel_dummy` is bound
there instead (`restir_gi_core.cpp:869-871,985`). Per pixel, word x holds the listed count and the low bytes of the
first picks' offsets (pick 0 x and y, pick 1 x), and word y holds pick 1 y, pick 2 x and y and min(scored, 255) in the
top byte (`shaders/restir_gi.glsl:842-856`). `RestirGiCore::read_selection` returns both layers; `run_case` adds
`result['selection']` when compatibility is on and sets `F_PROBE_STATS` then (`restir_harness.cpp:326,403`).

The `stats` status dictionary (present when `stats_valid`, i.e. with `readback`) includes truncated_candidate_share
(distance, cosine and Jacobian rejects over tested), truncated_energy_share, clamp_share (clamped pixels over geometry
pixels), pdf_refused, techniques_sample_unusable, lo_refused, no_canonical, temporal_shift_tested,
temporal_shift_rejected and spatial_combines. The temporal_shift_* fields sum the temporal row only; the other shift
fields sum the temporal and both spatial rows. Probe options d_min, cos_min, jac_max, vis_fail_limit and w_clamp
override the frame limits (values under Combine modes, "Similarity and shift limits"). The other keys are under
"Status dictionary".

The info texture holds the temporal reject reason in bits 0..3 and the spatial reject reason in bits 4..7, plus flags
in bit 8 temporal selected, 9 spatial selected, 10 visibility blocked, 11 visibility tested, 12 clamped, 13 had
history, 14 valid. Reason values: 0 none, 1 frame, 2 plane, 3 normal, 4 class, 5 epoch, 6 dirty, 7 jacobian,
8 distance, 9 cut, 10 no sample, 11 visibility, 12 no compat, 13 bound, 14 untraced (`restir_gi_inc.glsl:40-67`).
With `RESTIR_F_PROBE_PIXELS` the final pass also sets bit 15 and stores the candidate's luminance (at most a fixed 60000, not
the clamp setting) as a half in bits 16..31 (`restir_gi.glsl:1261`).

Reject reasons are written per pixel. Temporal (`rs_temporal_pixel`): 9 cut when history is not in use, 1 frame for
a previous position outside the frame, then the similar-test reason, 10 no sample for M = 0, 5 epoch, 6 dirty (B1 and
B2 history sample whose path hits a dirty box), then the reason from `rs_fill_neighbour` (11 visibility, 6 dirty, 8
distance, 7 jacobian for a cosine or |J| reject, 10 no sample for an invalid sample), and 13 bound when Fold clears
the history technique and no other reason is set. Pixels
that are not valid receivers or not traced write 0. Spatial (`rs_spatial_pixel`): the last neighbour's reason wins:
1 frame (outside or own pixel), 14 untraced, the similar-test reason, 10 no sample, and the `rs_fill_neighbour`
reason except 11; 12 no compat when compatibility selection picks nothing.

### Debug views (`modules/gi_restir/restir_gi.cpp`)

The backend maps a non-negative `RtgiFrameInputs::debug_view` d to module view 57 + d and renders it into the module debug
texture, which stays in the backend's resources (`debug_valid` in the status). `MODE_FINAL` implements views 57..63: 57 M/m_max, 58 age/32, 59 temporal/spatial selection, 60 temporal
reject reason, 61 log2 W, 62 sample visibility, 63 mode code (the mode, plus 4 with fresh Lo).
`servers/rendering/renderer_rd/environment/rtgi.cpp` leaves `debug_view` at its default -1, so the engine path never requests one;
harness case parameters carry the module view number (57..63) in `debug_view` directly.

### History cut from visibility (`modules/gi_restir/shaders/restir_gi.glsl`)

In the biased modes the temporal pass marks the history technique unusable when the stored `vis_state` low byte
exceeds `vis_fail_limit` (default 2) or bit 0x100 is set, giving it weight 0. The final pass, when `RESTIR_F_VIS` is
set, increments the low byte (capped at 255) on every failed visibility ray and sets 0x100; a successful ray clears
the state, and untraced pixels clear it. U1 skips this state check. The unusable technique stays present, and its M is
counted only in B2k.

### Lazy compilation (`modules/gi_restir/restir_gi_core.cpp`)

Shaders and pipelines compile on first use in `RestirGiCore::_ensure`. The NONE, SW and HW variants compile
separately, and the SW and HW variants also compile the sky-reread twins and SKY_EPOCH; `hw_variant_compiled()`
exposes the HW state. The fold-dump and lineage variants (`RS_FOLD_DUMP`, `RS_FOLD_LINEAGE`; TEMPORAL, plus its
sky-reread twin when the variant is not NONE) compile separately on first use in `RestirGiCore::_ensure_dump`
(`restir_gi_core.cpp:485`).

The first compile of a variant (`RestirGiCore::_ensure`) reads
`rendering/reflections/sky_reflections/texture_array_reflections` into `octmap_array` (`restir_gi_core.cpp:354`), so a
later change does not reach a variant already compiled. When it is true every ray variant adds
`USE_RADIANCE_OCTMAP_ARRAY` (`sky_radiance` becomes a `texture2DArray` read at layer 0) and the default sky texture is
`DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE`; otherwise `sky_radiance` is a `texture2D` and the default is
`DEFAULT_RD_TEXTURE_WHITE` (`_sky_texture`, `restir_gi_core.cpp:624-628`). Ray variants also define
`RAY_SCENE_BLAS_STACK_SIZE` and `RAY_SCENE_TLAS_STACK_SIZE` from `RayScene::BLAS_MAX_DEPTH` and `TLAS_MAX_DEPTH`, and
`RESTIR_RAYS`, followed by `RayScene::alpha_shader_defines()` (`RAY_SCENE_ALPHA` while
`rendering/global_illumination/ray_scene/alpha_test` is on, see `docs/code-notes/ray-scene-alpha.md`); the HW variant
adds `SHADER_GLSL_VERSION_460`, `GL_EXT_ray_query` and `RAY_SCENE_HW`; `VARIANT_NONE` gets no defines (`_defines`,
`restir_gi_core.cpp:335-347`).

### Harness and probe (`modules/gi_restir/restir_harness.cpp`)

`RestirProbe::_bind_methods` (`modules/gi_restir/restir_probe.cpp:49-80`) binds 30 methods: get_status, configure,
get_records, reset_records, plan_epochs, get_pixel_sums, reset_pixel_sums, request_capture, get_capture,
request_final_capture, get_final_capture, run_case, case_ready, get_case_result, read_case_file, write_case_file,
get_fold_rows, get_fold_row_fields, get_fold_seeds, get_fold_starts, reset_fold_starts, get_fold_starts_frames,
mirror_fold_starts (info, width, k, clock, pre_frame), run_fold_test, fold_test_ready, get_fold_test_result,
mirror_fold_test, get_fold_families (tag), get_fold_family_totals (tag) and reset_fold_families. `run_case(case)`
and `run_fold_test(args)` only queue the work on the render thread (`call_on_render_thread`); `case_ready` and
`fold_test_ready` turn true when it has finished, and `get_case_result` and `get_fold_test_result` return the result
(both are cleared when the next call starts). `request_capture` and `request_final_capture` clear the stored case and
set the `capture` or `capture_final` test option; `get_capture` and `get_final_capture` return the stored case.
`read_case_file` and `write_case_file` use `FileAccess` `store_var` and `get_var` without objects. The project setting
`gi/dump_path` (empty is off)
makes the live module write one frame once the state has `dump_after` frames (a `RestirTestOptions` field, default
24) with history in use. A case dictionary holds params, size, geo_pos, geo_nrm, prev_pos, prev_nrm, cand_first,
cand_second, the history reservoir (3 layers) and vis, each texture as `<key>_format` plus `<key>_l<layer>` byte
arrays. A MODE_CAPTURE pass copies the six input textures into module-owned textures created with copy-from usage,
and those are read back; geo_nrm and prev_nrm are stored as RGBA8 UNORM.

`params_to_dict` and `params_from_dict` (`modules/gi_restir/restir_harness.cpp:51-215`) carry `size`, `frame_index`,
`seed`, `flags`, `mode`, `spatial_count`, `spatial_radius`, `spatial_passes`, `m_temporal_cap`, `m_max`, `n_target`,
`epoch_tag`, `prev_epoch_tag`, `debug_view`, `vis_fail_limit`, `spatial_feedback`, `compat`, `compat_k`,
`compat_gate`, `compat_early_stop`, `camera_basis`, `previous_camera_basis`, `camera_to_anchor`,
`previous_camera_to_anchor`, `view_to_previous_clip`, `jitter_shift` and `previous_jitter_shift` (four floats each),
`test_sky`, `tol_plane`, `cos_normal`, `jac_max`, `d_min`, `cos_min`, `w_clamp`, `origin_scale`, `origin_floor`,
`sky_distance`, `class_tol`, `emission_exposure`, `dirty_count`, `dirty_margin`, `radiance_clamp`, `reval_growth`,
`reval_floor`, `compat_scale`, `compat_stop_score`, `sky_epoch_low`, `sky_epoch_high`, `sky_blur_lod`, `dirty_boxes`,
`clock`, `k_eff`, `pre_frame`, and `events` (only when `F_EVENTS` is set). `from_dict` reads `size` as `trace_size` (`run_case` then
sets it again from the case's own `size`), clamps `dirty_count` to 64, reads `dirty_boxes` as a `PackedFloat64Array`
of six values per box (min xyz, max xyz) and accepts `events` only as a byte array of exactly
`sizeof(RestirEventsUBO)`. Defaults when a key is absent (`RestirFrameParams`,
`modules/gi_restir/restir_gi_core.h:100-171`): `frame_index`, `seed`, `flags`, `epoch_tag`, `debug_view`,
`dirty_count`, `clock` and `k_eff` 0; `mode` 0 (B1); `spatial_count` 3;
`spatial_radius` 24; `spatial_passes` 1; `spatial_feedback` true; `compat` false; `compat_k` 16;
`compat_gate` 0 (similar); `compat_early_stop` true; `m_temporal_cap` 20; `m_max` 32; `n_target` 8;
`vis_fail_limit` 2; `test_sky` white; `tol_plane` 0.02; `cos_normal` 0.9063078 (25 degrees); `jac_max` 8; `d_min`
0.03; `cos_min` 0.05; `w_clamp` 20; `origin_scale` 2; `origin_floor` 0; `sky_distance` 1000; `class_tol` 0.35;
`emission_exposure` 1; `prev_epoch_tag` 0xFFFFFFFF; `dirty_margin` 0.01; `radiance_clamp` 60000; `reval_growth` 10;
`reval_floor` 0.1; `compat_scale` 0.12615662; `compat_stop_score` 0.5; `sky_epoch_low` 0.8; `sky_epoch_high` 1.25;
`sky_blur_lod` 2; `pre_frame` false; the bases and
`view_to_previous_clip` identity, the anchor offsets zero and the jitter shifts zero. They do not
carry `screen_size`, `trace_scale`, `reval_cut_m`, `reval_threshold`, `reval_share_denom`, the `shade_*` fields, `sky_reread`, `tile_skip` or `pack_m`,
so a replayed case runs with their defaults (and `run_case` has no rays anyway). A key absent from the dictionary
keeps the `RestirFrameParams` default, whose mode is B1, not the setting default B2.

run_case rebuilds the textures, clears history, loads the history and vis layers, and runs the pass chain with no ray
scene (`use_rays` false, so `VARIANT_NONE`). It returns radiance, coverage, debug and info bytes, the final reservoir
layers, SHA-256 hashes of radiance, coverage, reservoir and info, and the statistics. Result keys: `radiance`,
`coverage`, `debug`, `info`, `reservoir_l0` to `reservoir_l2`, `hash_radiance`, `hash_coverage`, `hash_reservoir`
(the three layers joined), `hash_info`, `stats`, `size`, `selection` (compatibility on) and `fold_dump` (`fold_dump`
test option on); a failed allocation returns only `error` ("allocation failed"). `stats` holds `geometry`,
`output_pixels`, `temporal_selected`, `spatial_selected`, `clamped`, `violations`, `m_sum`, `compat_taps`,
`compat_early_stops`, `compat_empty`, `compat_h_sum`, `spatial_combines`, `techniques_present`, `fold_refused`,
`fold_carried`, `fold_renewed`, `fold_started` and `fold_refused_empty`. `fold_dump` holds `active`, `k_eff`,
`clock`, `n_valid`, `cut_rej`, `had_history`, `hp_eq_px`, `expected_phase0`, `phase0_start`, `refused_off`, `s_max`,
`n_over`, `texels` and `starts` (the last four are -1 and empty when the dump did not run). `run_case` applies the
`neighbor_selection`, `compat_k` (1 to 64), `compat_gate` (0 to 2) and `compat_early_stop` test options over the
case's params (`modules/gi_restir/restir_harness.cpp:312-331`). `capture_final` copies the final
reservoir layers, geo_pos, geo_nrm, radiance, coverage, debug and info after the next frame's final pass.

`gi/deterministic` makes `rs_rand` use frame 0 and seed 0x1234567 (`rs_rand_frame` keeps the real frame) and turns
spatial offsets into a constant spiral: angle k * 2.39996 + 1.3 * pass_index, length
radius * max(sqrt((k + 0.5) / spatial_count), 0.15). `pass_index` is 0 for the temporal pass and 1 or 2 for the
spatial passes; the selection random dimension is `i + 16 * pass_index`, so an external reference has to use
pass_index 1 for the first spatial pass.

### Test options (`modules/gi_restir/restir_test.h`)

`RestirProbe::configure` (`modules/gi_restir/restir_probe.cpp:102-189`) sets these `RestirTestOptions` fields
(defaults in brackets); a key left out keeps its value:

- `readback` (false): reads the statistics back and records every frame.
- `history` (true): off makes `use_history` false.
- `spatial_feedback` (-1, from the mode: on only in U1; 0 off, above 0 on), `neighbor_selection` (-1; 0 offsets, 1
  compatibility), `compat_k` (-1; above 0 applies, clamped 1 to 64), `compat_gate` (-1; 0 similar, 1 none, 2 binary)
  and `compat_early_stop` (-1; 0 off, above 0 on).
- `pixel_detail` (false): sets `F_PROBE_PIXELS`.
- `sky` (a colour, default white; also sets `constant_sky`, default false) and `real_sky` (`constant_sky` = not
  `real_sky`); `sky` always feeds `test_sky`. `constant_sky` stops the shade set's exposure from replacing
  `emission_exposure` and, with the own sampler, turns the real sky off (`restir_gi.cpp:549,654-667`).
- `d_min`, `cos_min`, `jac_max`, `w_clamp` and `vis_fail_limit` (all -1, unset): `d_min` and `cos_min` apply from 0,
  `vis_fail_limit` from 0, `jac_max` and `w_clamp` only when above 0 (`restir_gi.cpp:673-687`); `w_clamp` overrides
  the W clamp factor.
- `accumulate_from` and `accumulate_to`: the record-index window for `get_pixel_sums`.
- `seed_offset`: added to `rtgi/seed`. `rearm_dump`: sets `dumped` false so `gi/dump_path` writes again.
- `fold_tag` (0), `release_once` (false), `fold_dump` (false; true also sets `readback`; false clears
  `fold_lineage`) and `fold_lineage` (false; true without `fold_dump` stays false and records the refusal
  `RESTIR_FOLD_LINEAGE_REFUSAL_NO_DUMP`).
- `freeze`: with a state that has history, `resample` returns the stored radiance and coverage
  before it builds `RestirFrameParams` and before `frames++`. Only when `gi/denoise` is r2 does it run `run_pack`,
  against the flag word last uploaded (`resources.ubo[3]`, from the last real frame) with this frame's tile texture, so
  a tile setting changed during a freeze takes effect after it ends. Its `history_cap` is the clamped
  `gi/denoise_history_cap` without the ramp, and `gi/fold_k` is not read, so a Fold change takes effect and cuts on the
  first frame after the freeze (`modules/gi_restir/restir_gi.cpp:425-455`).

`capture`, `capture_final`, `dumped` and `dump_after` (24) are set by other calls or by the backend; `capture_final`
is set by `request_final_capture`.

### Backend gates (`modules/gi_restir/restir_gi.cpp`, `RestirGiBackend::begin_frame`)

`begin_frame` refuses a frame (returns false, raises the backend's own `refusals`, sets `refusal_reason`; the status
reports them as `refuses`, `refusals` and `refusal_reason`, `restir_gi.cpp:935-938`) at the first failing check, in
this order: no buffers; missing inputs (the trace size is not positive, or a
candidate or geometry texture is null); the selected shader variant did not compile (the hardware variant has its own
wording and also marks the ray scene's hardware path refused, `RayScene::refuse_hardware`); `gi/own_sampler` is on
without a ready ray scene ("own sampler needs the ray scene"); with `gi/event_reshade` on, the four event refusals in
the order under "Events (gi/event_reshade, default off)" (`restir_gi.cpp:373-384`); allocation failed. The
`release_once` test option is consumed after the event checks and before the state is allocated (see "Run counters",
release_once). `refusal_reason` is cleared at the start of each `begin_frame`, which then calls `_check_choices`
(see Settings) before the first check (`restir_gi.cpp:353-399`).

### Status dictionary (`modules/gi_restir/restir_gi.cpp`, `RestirGiBackend::get_status`)

Top-level keys not named elsewhere in these notes: `ready` (a frame has run), `states` (the count of per-view states),
`hw_variant_compiled`, `invalid_choice` (see Settings), `lo_clamp` (`gi/radiance_clamp` as applied, default 60000),
`mode` (int), `denoise`, `frame`,
`history_used`, `own_sampler`, `rays`, `hardware`, `spatial_neighbors`, `spatial_passes`, `m_temporal_cap` and `m_max`
(as applied after the rate, ramp and light caps), `cut` (bool), `history_cap`, `reval_active`, `reval_frames` (the
state's re-validation frame count), `trace_size`, `stats_valid`, `debug_valid`, and `allocations` (an array of name,
`expected_bytes`, `measured_bytes` and `buffer`, from the `alloc_log` of the first state only).

The `stats` dictionary exists only when `stats_valid` (readback). Besides the keys listed under "Statistics" it holds
the raw counts `geometry`, `output_pixels`, `m_sum`, `clamped`, `blocked`, `had_history`, `temporal_selected`,
`spatial_selected` and `violations`, and `candidate_pixels`, `candidate_sum`, `output_sum`, `has_sample`,
`shift_tested`, `truncated_distance`, `truncated_cos`, `truncated_jacobian`, `energy_all`, `energy_truncated`,
`rehit_miss`, `rehit_mismatch`, `reshaded`, `reshade_overflow`, `reshade_edge_stale`, `event_capped`, the `fold_*`
counts, the `compat_*` counts, and `temporal_reject` and `spatial_reject` (16-entry arrays counted from info words
with `INFO_VALID`) (`restir_gi.cpp:934-1124`).

## Invalidation and R2 integration (`modules/gi_restir/restir_epochs.cpp`)

`restir_plan_epochs()` in `modules/gi_restir/restir_epochs.cpp` turns `RestirEpochInputs` into a `RestirEpochPlan`
(both declared in `modules/gi_restir/restir_epochs.h`). Inputs: the history flags, the previous and current
`GIEpochs::Snapshot`, and the dirty feed, which reaches the module through `RtgiFrameInputs.dirty_source`
(`servers/rendering/renderer_rd/environment/rtgi.cpp` fills it with `GIEpochs::get_singleton()`). "R2" in these
notes is that host RTGI pass.

**Cut order** (`restir_plan_epochs`). The first matching rule sets `cut` and the reason; later rules are not reached.

1. No history: `RESTIR_CUT_FIRST`.
2. `history_cut`: `RESTIR_CUT_REQUEST`. `rtgi.cpp` sets `FRAME_FLAG_HISTORY_CUT` for every R2 cut (resize, camera
   cut, backend switch and the rest). The module input is true when `FRAME_FLAG_HISTORY_CUT` or
   `FRAME_FLAG_CAMERA_CUT` is set, and on the first backend frame of a render buffer set.
3. A hardware backend change, or a change in whether sky re-read applies: `RESTIR_CUT_BACKEND`.
4. A Fold change: `RESTIR_CUT_FOLD`; the condition is `restir_fold_cut` under Fold, "Clock, pre-frame and cuts".
5. An origin epoch change: `RESTIR_CUT_ORIGIN`.
6. A world epoch change: `RESTIR_CUT_WORLD`.
7. A scene epoch change: resolved against the dirty feed, below.

**Scene epoch and the feed** (`servers/rendering/renderer_rd/environment/ray_scene.cpp`, `GISceneTracker::update`
in `gi_epochs.cpp`). The scene epoch moves on the tracker's first update and whenever its signature changes. The
signature sums a per-instance hash of the mesh RID, the world transform, the layer mask, the mesh build serial and
the packed material block. An instance added, removed, moved, rebuilt, re-masked or re-materialled moves it, and a
mesh rewritten every frame moves it every update. A scene change alone is not a cut. The world epoch moves when the
signature changed, the previous update's member set (instance and mesh) was not empty and differed, and none of
its members is still present. When the stored scene epoch differs from the current one and the feed is live, the
module calls `GIEpochs::get_dirty_boxes(stored scene epoch)` through `dirty_source` and the planner applies:

- no live feed: `RESTIR_CUT_SCENE_NO_FEED`.
- `DIRTY_NONE`: no cut (a change with no geometry behind it).
- any other result besides `DIRTY_BOXES`, or more than `RESTIR_DIRTY_MAX` (64) boxes: `RESTIR_CUT_SCENE_OVERFLOW`.
- `DIRTY_BOXES` with no box, a non-finite box, or a negative size on an axis: `RESTIR_CUT_SCENE_UNLOCALISED`.
- otherwise the boxes become the plan's, each shifted by minus `RayScene::get_anchor()`. Boxes are world space;
  the shader works in anchor-relative space. They reach the shader in `RestirDirtyUBO` (2048 bytes: 64 `vec4`
  mins, 64 `vec4` maxes).

`RESTIR_CUT_MAX` is 10, so `cut_counts` holds one slot per reason value, including `RESTIR_CUT_NONE`. A
`RESTIR_CUT_BACKEND` cut with an unchanged hardware flag also increments `sky_reread_cuts`.

**Probe planner** (`RestirProbe::plan_epochs`, `modules/gi_restir/restir_probe.cpp:473-522`). `plan_epochs(case)` runs
`restir_plan_epochs` on the CPU with no rendering device. Case keys (defaults): `current` and `previous` snapshots
(`scene`, `light`, `sky`, `origin`, `world`, each 1), `have_history` (true), `have_epochs` (true), `history_cut`,
`hardware_changed` and `fold_changed` (false), `anchor`, `light_soft_frames` (0), `m_light_frames` (4), `have_feed`
(false), `feed_boxes` (6 doubles per box: min xyz then size xyz) and `feed_result` (default `DIRTY_OVERFLOW`). Result
keys: `cut`, `reason`, `dirty_count`, `dirty_boxes` (anchor-relative min and max, 6 per box), `scene_changed`,
`light_changed`, `sky_changed`, `origin_changed`, `world_changed` and `light_soft_frames`.

**Feed ownership** (`modules/gi_restir/restir_gi.cpp`). `RestirGiBackend::resample` takes `RayScene::FEED_MODULE`
when it has a ray scene. `release` gives it back when the last per-view state is dropped, or when `release(nullptr)`
frees all states (it also frees the core). `RayScene::feed_frame_needs_epoch` reports a frame that has changes or a
raw overflow while the tracker did not move the epoch itself; `RayScene::_feed_end` (`ray_scene_dirty.cpp`) then
moves the scene epoch. A hardware-usability flip is a change the feed sees and the tracker's hash does not.
`GIEpochs::get_dirty_boxes` collects only frames whose scene epoch is newer than the reader's epoch, so a frame
with changes needs an epoch step of its own.

**State and status** (`restir_gi.h`, `RestirGiBackend::get_status`). Per state: `cut_counts[reason]`,
`sky_reread_cuts`, `frames_since_cut`. `get_status` sums `cut_counts` over all states and reports `cut_counts`,
`cut_reason`, `dirty_boxes` (0 when the frame did not use history), `dirty_feed`, `scene_changed`, `light_changed`, `sky_changed`, `origin_changed`, `world_changed`,
`light_cap` and `light_frames_left`.

**What moves a box** (`RayScene::_feed_observe`, `RayScene::feed_surface_state` in `ray_scene.h`). Per instance the
feed compares the world transform, the mesh RID and build serial, hardware usability, and a state word hashed from
the layer mask and every surface's material flags (shadow casting included) and coverage. A change of who a mesh
shadows, or of how much it lets through, dirties its bounds like a move does. An albedo or emission change alone
moves the scene epoch, because the packed material block is in the tracker hash, but moves no box: that frame
reads `DIRTY_NONE`, history is kept, and the re-validation rays, where they run, follow the change.

**Dirty test** (`modules/gi_restir/shaders/restir_gi.glsl`, `rs_dirty_hit`, temporal pass only). The segment runs
from the previous receiver point `q.pos` to the stored point `x_s`, or to `q.pos + xs * sky_distance` for a sky
sample (`xs` holds the direction); with no valid stored sample it is a single point. Every box is grown by
`dirty_margin` (`gi/dirty_margin`, default 0.01 m) and tested with
`rs_segment_hits_box`.

- A hit gives reject code 6 (`REJ_DIRTY`). In `gi/mode` B1 and B2 the history sample is dropped, so M comes from
  the fresh canonical only. In B2k (`F_KEEP_M`) its M is kept and the sample is not used. In U1 the sample is not
  used but its M still enters the combine, and a box hit on the canonical sample's segment, seen from the previous
  receiver, zeroes the history candidate's `a_at_c`. The pixel keeps `INFO_HAD_HISTORY`.
- The spatial pass has no dirty test: `rs_dirty_hit` returns false outside `MODE_TEMPORAL`.
- Fold-dump variants: the fold-dump and lineage shaders (`RS_FOLD_DUMP`, with `RS_FOLD_LINEAGE`) are also built
  with `MODE_TEMPORAL` (`restir_gi_core.cpp:494-502`) and carry the same test; when they replace the temporal
  dispatch is under "Fold dump and self-test".
- Epoch tag: a stored sample whose epoch tag differs from `prev_epoch_tag` is rejected with code 5 (`REJ_EPOCH`).
  `epoch_tag` (`restir_gi.cpp`) packs the low byte of the scene, light and origin epochs (bits 0, 8, 16), and
  `prev_epoch_tag` is the previous frame's `epoch_tag`. `RestirFrameParams.prev_epoch_tag` is `0xFFFFFFFF` when
  there is no history, and the uniform word then takes the current `epoch_tag`.

**Light epoch** (`restir_gi.cpp`, `restir_plan_epochs`). A light epoch change fires no cut. It sets
`light_soft_frames` to `gi/m_light_frames` (0..64, default 4; 0 disables it, and the event path, `gi/event_reshade`
with `gi/replay_seed`, forces 0 and zeroes the counter) and for that many frames clamps `m_temporal_cap` and
`m_max` down to `gi/m_light` (1..32, default 2), decrementing on each of them.

**Sky epoch.** The planner clears nothing and `sky_changed` is only reported (the brightness-driven sky cap
described under `gi/sky_reread` is separate and needs `gi/event_reshade`). Without `gi/sky_reread` a `MISS_SKY`
sample keeps its stored Lo until it is replaced, or until the non-replay re-validation pass refreshes it. With
`gi/sky_reread` the module re-reads the sky by direction from the DDGI sky texture (set 2, bindings 7 and 8) in the
temporal, spatial and final passes.

**Module off** (`RtgiPass::_process` in `rtgi.cpp`). When `rendering/global_illumination/restir/gi/enabled` is off,
`_process` releases the module's state for the render buffers it is drawing. That frees the history textures and,
with the last state, returns the feed. Enabling it again starts with no history, so the first frame cuts with
`RESTIR_CUT_FIRST`.

## Integration with the R2 pass (`modules/gi_restir/shaders/restir_gi.glsl`)

`modules/gi_restir/shaders/restir_gi.glsl` and `modules/gi_restir/restir_gi_core.cpp` are the module side;
`servers/rendering/renderer_rd/environment/rtgi.cpp` and `rtgi.h` are the host side.

- Rays: every trace is `ray_scene_trace` with an explicit seed. Each call site derives it from the pixel and the
  frame plus its own constant; the replay paths pass the stored seed instead.
- Backface: a hit is shaded when it faces the ray, or when its material is two-sided or a blend material, as in
  `ray_scene_hit_shade_inc.glsl` (`shade_hit` returns alpha `-t` otherwise). In the non-replay paths a rejected
  backface gives zero radiance; the non-replay SOURCE shade also sets `RTGI_FLAG_BACKFACE`.
- Hardware ray scene: when `RayScene::uses_hardware()` is true the module uses `VARIANT_HW`, built with
  `RAY_SCENE_HW` and `GL_EXT_ray_query` (`RestirGiCore::_defines`). Both variants bind the ray scene's buffers
  through `RayScene::append_uniforms`. If the hardware variant or one of its pipelines cannot be made,
  `RestirGiBackend::begin_frame` refuses the frame with "hardware shader variant did not compile" and calls
  `RayScene::refuse_hardware`, as DDGI does.
- Bindings: the shade set is set 2 and uses binding 0 and bindings 3 to 8 only, so it has no cloud-shadow input.
- Output route: only `gi/denoise = r2` is accepted. Any other value leaves the frame's `denoise` false;
  `rtgi.cpp` counts the frame in `backend_refusals` and runs its own candidates.
- Tiles: `wants_all_tiles()` and the tile skip are under "Traced tiles only and M in PACK"
  (`servers/rendering/renderer_rd/environment/rtgi.cpp:383-384`).
- Per-view release: `RtgiBuffers::free_data` calls the module's `release(owner)` for its render buffers;
  `RtgiPass::release` calls `release(buffers)` and then clears the RTGI context.

## Replay (`gi/replay_seed`, default off)

The replay contract spans `servers/rendering/renderer_rd/environment/rtgi.cpp`,
`servers/rendering/renderer_rd/shaders/environment/ddgi_trace.glsl`,
`servers/rendering/renderer_rd/shaders/environment/ray_scene_inc.glsl` and
`modules/gi_restir/shaders/restir_gi.glsl`.

**R2 side.** `wants_replay()` returns `gi/replay_seed`. RTGI then sets `FRAME_FLAG_REPLAY` (512) in the trace UBO's
flags only; the frame UBO does not receive it. `ddgi_trace.glsl` under `MODE_RTGI` defines `RAY_SCENE_REPLAY` and
sets `ray_scene_replay` from that bit in `main`. With the bit set, `RAY_SCENE_LIGHT_KEY` keys light seeds by light
id instead of index, `RAY_SCENE_PICKS` uses the race pick, and, when the mirror path is on
(`mirror_below_surface` set and `dot(offset_normal, normal) > 0.999`), the stored pdf is `p0(w) + p0(m(w))` with
both cosines floored at 1e-4. With the bit clear the pick is index-keyed and the pdf is `p0(w)`.

**Module side.** In the ray variants `MODE_FINAL`, `MODE_SOURCE` and `MODE_VALIDATE` define `RAY_SCENE_REPLAY`, and
`rs_shade_setup()` sets `ray_scene_replay` from `F_REPLAY` (8388608) before every replay shade, so a re-shade takes
the same id-keyed race, area-light point and shadow seeds as R2's candidate shade. The non-replay paths never call
`rs_shade_setup`, so the global stays false there.

**Seed at import.** With replay on and `gi/own_sampler` off, the temporal pass overwrites each imported candidate's
tag with `ray_scene_hash(pixel.y * 65537 + pixel.x + frame * 6271)`, where `pixel = min(trace_pixel * trace_scale +
trace_scale / 2, screen_size - 1)` (`rtgi_anchor_pixel`, `rtgi_shade_key`) and `frame` is R2's `frame_index` (both
UBOs take it from the same `FrameInputs`). With replay and `gi/own_sampler` on, SOURCE writes its own seed
`ray_scene_hash(px.y * 65537 + px.x + frame * 6271)` into the candidate tag, traces with that same seed, and the
import keeps the tag. `RestirFrameUBO` (size under "Storage and frame buffers", `restir_gi_core.cpp:114`) has its
`screen_size` (ivec2), `trace_scale` and `replay_pad` words in one `uvec4` row that `restir_gi_inc.glsl` must mirror.

**Shared shade.** `restir_gi.glsl` includes `restir_core_ray_scene_hit_shade_inc.glsl`, which includes
`ray_scene_hit_shade_inc.glsl`, with four macros: three are bound to globals that `rs_shade_setup()` fills. Max
distance, bounce energy and emission exposure take the DDGI values (`ddgi.max_distance`, `rf.shade_bounce_energy`,
`rf.shade_exposure`) when DDGI shading is on and the own sampler is off, else `rf.sky_distance`, 0 and
`rf.emission_exposure`. Max radiance is `RS_LO_CLAMP` (`rf.radiance_clamp`, default 60000, the value of `DDGI_MAX_RADIANCE`).
The non-replay paths keep their own shading (`rs_fresh_lo` and SOURCE's inline shade).

**VALIDATE in replay mode** (`rs_validate_replay`, `rs_replay_rehit`, `rs_replay_shade`). A short re-hit runs from
`x_s + n_s * eps` along `-n_s` for `2 * eps`, with `eps = 0.02 + 0.002 * d` (`d` from the previous receiver) and the
stored seed. The hit counts only when `|t - eps| < 0.5 * eps` and its facing normal has `dot > 0.99` with `n_s`. The
sample is then shaded from the previous receiver with `t = |x_s - o_prev|` and the stored seed, and the re-shaded
Lo, the re-hit's facing normal and the backface bit (shade alpha below 0) are written. A miss or a mismatch writes
Lo = 0, keeps M and the flags, and counts as `PS_REHIT_MISS` or `PS_REHIT_MISMATCH` (pass stats row 3, slots 16 and
17). A non-finite re-shade writes Lo = 0 uncounted. This path has no growth limit and no change cut, and it
leaves sky samples alone. The share selection uses `rs_rand_frame`, which keeps the frame in deterministic mode.

**FINAL with `gi/fresh_lo` in replay mode** traces from the receiver with the stored seed of a non-sky sample and
shades through the shared shade (`rs_fresh_lo_replay`). `gi/fresh_lo` is ignored, with a warning, unless the own
sampler or the DDGI shading inputs are present (`restir_gi.cpp:544-547`).

**Pass stats.** Layout and rows (`PS_STRIDE` 32, `PS_ROW_VALIDATE` = 3) are under "Statistics"
(`modules/gi_restir/restir_gi_core.cpp:117-118`).

**Setting timing.** Set `gi/replay_seed` before a run. Changing it mid-run cuts no history: samples stored before
the change carry R2's pixel-and-frame tag, which a replay re-shade reads as a seed, so they use other draws until
the history turns over. `wants_replay()` must read a setting, because RTGI reads it before `begin_frame` to set the
trace flag, so it cannot depend on anything the module decides inside the frame.

**Mirror floor** (`ddgi_trace.glsl`, `MODE_RTGI`). Each pdf term is `max(cos, 1e-4) / pi`. When the mirrored
direction lies below the shading hemisphere its cosine is not positive, so the mirrored term is the 1e-4 floor,
where the true `p0(m(w))` is 0. The added density is negligible against `p0(w)` except near grazing, where `p0(w)`
approaches the same floor; because `p0(w)` is itself floored there, the pdf can reach about twice `p0(w)`.

**Macro contract** (`ray_scene_inc.glsl`). Under `RAY_SCENE_REPLAY` it declares `ray_scene_replay` and defines
`RAY_SCENE_LIGHT_KEY` and `RAY_SCENE_PICKS` to follow it, and a light whose weight is below `RAY_SCENE_MIN_TARGET`
is skipped. Any other shader that wants the id-keyed pick must define that macro and set the global from its own
frame flags; where the macro is undefined the key is the light index and the pick is index-keyed.

## Events (gi/event_reshade, default off)

`modules/gi_restir/restir_events.cpp` plans the event path and `modules/gi_restir/restir_gi.cpp` runs it;
`servers/rendering/renderer_rd/environment/rtgi.cpp` supplies the inputs.

- `gi/event_reshade` is off by default and needs `gi/replay_seed`. `RestirGiBackend::begin_frame`
  (`restir_gi.cpp`) refuses a frame with it on, in this order, with: "event_reshade needs replay_seed"
  (`gi/replay_seed` off), "event_reshade needs the ray scene" (no ready ray scene), "event_reshade needs the
  DDGI shading inputs" (`RtgiFrameInputs::shade.valid` false) and "event_reshade does not run with revalidate
  budget" (`gi/revalidate` is budget). A refused frame leaves RTGI on its own path.
  `gi/revalidate` off and share are allowed.
- `events_on` (`resample`) is `gi/event_reshade && gi/replay_seed`. It zeroes `state.light_soft_frames` and passes
  `m_light_frames = 0` to the epoch planner, so `light_cap` is false and `gi/m_light` never caps; `light_changed` is
  still reported. With events off the light soft frames follow "Invalidation and R2 integration", Light epoch.
- `RtgiFrameInputs` (`servers/rendering/renderer_rd/environment/gi_module_hooks.h`) carries `events`,
  `event_count`, `events_lost`, `directional_jump` and `restart_quiet_frames`.
- `RtgiPass::_read_light_changes` (`rtgi.cpp`) calls `DDGILightTracker::events_since` with the view's
  `RtgiBuffers::event_serial`, copies each event anchor-relative into `backend_events`, and sets
  `backend_events_lost` to `serial - asked - events.size()`. `RtgiBuffers::events_lost` stays cumulative.
  While a view has no previous frame, `asked` is `UINT64_MAX`: no events arrive and nothing is lost.
- `directional_jump` is `RtgiRestarts::read_directionals(...).jumps > 0`; repeats do not set it.
  `restart_quiet_frames` is `rendering/global_illumination/rtgi/restart_quiet_frames`, clamped 0..1024,
  default 0.
- `DDGILightTracker` (`servers/rendering/renderer_rd/environment/ddgi_light_tracker.h`) has one ring of
  `MAX_EVENTS` = 64 slots with no view tag; each `RtgiBuffers` has its own cursor. More than 64 events
  between two reads of a view make `events_lost` positive, and such a frame is a jump frame whichever view
  wrote the events. A changed light fires at most once per `RATE_FRAMES` = 8 frames, as its previous and its
  current state; both reach the module as spheres and exact duplicates are not dropped. A light that appears or
  disappears fires once with `since_last` UINT32_MAX and is not rate limited (`ddgi_light_tracker.cpp:216`,
  `:222` and `:247`). The tracker lists events whenever `rendering/global_illumination/rtgi/enabled` is on
  (`ray_scene.cpp:1280`), so `events_read` counts them with `gi/event_reshade` off too.
- `resample` copies the vector into `RestirEventSphere` (reach is the tracker's `half`) and then into
  `RestirEventsUBO`, so the host vector is valid only for that call.
  `static_assert(RESTIR_EVENT_MAX >= DDGILightTracker::MAX_EVENTS)`; `RESTIR_EVENT_MAX` is 64.
- The epoch snapshot is taken in `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`
  before `_read_light_changes` bumps `GIEpochs::SOURCE_LIGHT`. The module sees a light's events on frame N
  and its light epoch change on frame N+1.
- The module does not change RTGI's restarts. `servers/rendering/renderer_rd/shaders/environment/rtgi.glsl` applies
  its soft cut and restart spheres in its temporal pass (`rtgi.glsl:437-446`) only while
  `RTGI_FRAME_FLAG_MODULE_OUTPUT` is clear. When RTGI skips its restarts and which module output is requested are
  under "R2 side: module output, route coverage, tile memory", Hook fields
  (`modules/gi_restir/restir_events.cpp:312-322`).
- `RestirEventsUBO` (`modules/gi_restir/restir_events.h`) matches the std140 block `RestirEvents` in
  `modules/gi_restir/shaders/restir_gi_inc.glsl`: set 0 binding 26, 2368 bytes. The shader declares it for
  TEMPORAL, SPATIAL, VALIDATE and SKY_EPOCH. TEMPORAL and SPATIAL bind it every frame, VALIDATE when it
  dispatches, SKY_EPOCH when it runs. `RestirGiCore::run_frame` (`restir_gi_core.cpp`) writes zeros when
  F_EVENTS is clear.
- `counts` = (event count, unused and always 0, `event_m_cap`, `jump_m_cap`). `restir_plan_events` writes the
  event count into counts[0]; the first-fire count is `RestirEventPlan::capping` (status
  `event_capping_spheres`). Only events are packed, so slots past the count are unread.
- `params` = (`gi/event_reach_scale`, `gi/event_solid_angle`, 0, 0): ranges 0..64 (default 2.0) and
  0..12.566371 (default 0.05). `gates` = (quiet frames, budget B, jump frame 0 or 1, VALIDATE selection:
  0 none, 1 event, 2 all). `gi/event_m_cap` and `gi/jump_m_cap` are 0..256, default 0.
- `sphere[i]` = (anchor-relative centre, reach). `since_last` is a uvec4[16] holding sphere i at
  `[i >> 2][i & 3]`. Slots after the events are unused: the planner's dirty boxes go to the dirty UBO and
  the shader's own `rs_dirty_hit`, not here. `held[64]` and `hold[0]` belong to the SPATIAL hold below.
- The frame UBO (`RestirFrameUBO`, block `RestirFrame`; size under "Storage and frame buffers",
  `restir_gi_core.cpp:114`). F_EVENTS is flags bit 24. Bit 19 is Fold's F_PRE_FRAME and bit 22 is B2k's F_KEEP_M. The
  jump frame is `gates[2]`, not a flag.
- A captured case carries the UBO as "events" when F_EVENTS is set (`modules/gi_restir/restir_harness.cpp`).
  The loader accepts only a blob of exactly `sizeof(RestirEventsUBO)`.
- First fire is `since_last >= quiet_frames`; `UINT32_MAX` always fires. The receiver cap
  (`restir_event_caps` in `restir_events.cpp`, shader `rs_event_caps`) runs over first-fire events only:
  `d = |x - c| < reach_scale * R`, or the solid angle `2*pi*k*k/(1 + sqrt(1 - k*k))` with `k = R/d` exceeds
  `event_solid_angle`. `d <= R` or `d < 1e-6` counts as 4*pi; visibility is ignored.
- The sample test (`restir_event_holds`, shader `rs_event_holds`) is `|x_s - c|^2 <= R^2` over every event,
  first fire or repeat, with the raw reach; sky samples are excluded before it.
- TEMPORAL (`rs_event_cap_history`, `modules/gi_restir/shaders/restir_gi.glsl`) takes
  `m_h = min(h.m, m_temporal_cap)` (the cap after rate scaling, the confidence ramp and the light cap), then
  `min(m_h, event_m_cap)` when the receiver passes the cap test, then `min(m_h, jump_m_cap)` when `gates[2]`
  is set or the sky epoch jump word is set. The history stays present with M 0 and therefore zero weight;
  no reject code is added.
- SPATIAL (`rs_spatial_pixel`, `restir_gi.glsl`) treats the receiver as capped when it passes
  `rs_event_caps` (with F_EVENTS) or `rs_hold_caps` (with F_SPATIAL_HOLD). A neighbour that passes neither
  takes `M = min(min(n.m, m_max), event_m_cap)`; a neighbour inside either region keeps its M. The jump
  cap is TEMPORAL only.
- A jump frame (`RestirEventPlan::jump_frame`) is `directional_jump` or `events_lost > 0`. The candidates are kept and
  F_REVAL_FRAME is never set with events on. With history in use, `jump_m_cap` 0 selects no VALIDATE work and above 0
  selects ALL. A sky epoch jump is not a jump frame; see "Sky re-read (gi/sky_reread, default off)"
  (`modules/gi_restir/shaders/restir_gi.glsl:382-386`).
- EVENT selection (VALIDATE, `restir_gi.glsl`) runs when events are on, the history is used, the ray scene
  is ready, the frame is not a jump frame and it has events. Early outs (`rs_event_load`): M 0, RF_VALID
  clear, RF_SKY, an epoch tag that differs from the previous frame's, an invalid previous receiver, or
  `|x_s - o_prev| < 1e-3`. There is no tile skip and no skip on this frame's NO_CANDIDATE. With
  `event_m_cap` 0, a flagged sample whose previous receiver passes the cap test is not re-shaded
  (`rs_event_flagged`). The re-shade is `rs_validate_replay`; a share pick in the same dispatch re-shades a
  pixel once even when it is also flagged.
- ALL selection (jump frame, `jump_m_cap > 0`) re-shades every sample that passes the same load test.
- `B = floor(trace_w * trace_h * gi/reshade_budget / 100)` (`restir_event_budget`); the percent is clamped
  to 0..100, default 25. The key is `rs_rand_frame(px, RS_EVENT_KEY_DIM)` with `RS_EVENT_KEY_DIM` 200: it
  changes every frame, and no other draw in `restir_gi.glsl` uses a dimension of 200 or more (the others
  stay under 176).
- On an EVENT-selection frame `run_frame` clears the 256-entry (`RESTIR_EVENT_BINS`, 1 KB) storage buffer
  at set 0 binding 28 and dispatches the same VALIDATE shader with `pass_index` 4
  (`RESTIR_PASS_EVENT_COUNT`); each flagged pixel adds one to `bin = min(uint(key * 256), 255)`. The main
  dispatch computes, once per work group, the first bin at which the running sum would pass B
  (`restir_event_cut_bin`), and re-shades flagged pixels with `bin < cut`. The taken count is at most B, is
  all flagged pixels when that count is at most B, and otherwise falls short of B by less than one bin's
  count. A flagged pixel not taken keeps its stale Lo and counts `reshade_overflow`.
- Frame order with events on (`restir_gi.cpp`, `restir_gi_core.cpp`): plan epochs (`m_light_frames` 0),
  plan events and pack the hold into the UBO; `run_source` when the own sampler is on and the frame is not
  F_REVAL_FRAME; `run_frame` then runs VALIDATE (preceded by the count dispatch on an EVENT-selection
  frame), the sky epoch pass when on, TEMPORAL, SPATIAL and FINAL; `run_pack` runs last when `gi/denoise`
  is r2. With `gi/event_reshade` off the event UBO is zero, F_EVENTS is clear and no count dispatch runs,
  so every pass takes its non-event branch.
- With a fold dump buffer the TEMPORAL dispatch is replaced by the dump or lineage build; see "Fold dump and
  self-test" (`modules/gi_restir/restir_gi_core.cpp:938-966`).
- Status always (`restir_gi.cpp`): `event_reshade`, `events_read` (delivered this frame, also when events
  are off), `events_lost` (this frame's missed events), `jump_frame`, `event_capping_spheres`,
  `event_validate` and `reshade_budget_pixels`. Backend-lifetime totals: `event_frames_total` (frames with
  `events_read > 0`, also when events are off), `events_read_total`, `event_validate_frames_total` and
  `event_capping_total`.
- Probe stats only (read back with the readback test option; layout under "Statistics"): `reshaded` (PS row 3 slot 19,
  event or ALL re-shades; share picks are not counted), `reshade_overflow` (row 3 slot 20, flagged not taken and not
  share-picked, plus `reshade_edge_stale`), `reshade_edge_stale` (row 0 slot 21: EVENT selection, `event_m_cap` 0, a
  valid non-sky history in an event sphere, the previous receiver capped, the current one not) and `event_capped` (row
  0 slot 18: history M above the event cap).
- Tests in `modules/gi_restir/tests/test_restir_events.h` cover: budget flooring and the 100 percent cap;
  first fire against the quiet window; the solid angle formula; the receiver cap; repeats re-shading
  without capping; the sample test's raw reach; bins and the budget cut; an inactive plan; packing and
  selection, including a full 64-sphere plan; and jump frames (directional, lost, and the jump cap).

## SPATIAL hold (gi/spatial_hold, default uncapped)

`modules/gi_restir/restir_events.cpp` owns the ring and `modules/gi_restir/restir_gi.cpp` updates and
packs it.

- The hold runs only with the event path on (`gi/event_reshade` and `gi/replay_seed`), a ready ray scene,
  and `gi/spatial_hold` = `uncapped` (the default; `off` disables it). `gi/spatial_hold_frames` is 1..256,
  default 40. Without a ready ray scene the ring is cleared and nothing is stored, because event centres
  are relative to the ray scene anchor. Dirty boxes are not events and are not held.
- Each view's `RestirGiState` keeps a `RestirHoldRing` of `RESTIR_HOLD_MAX` = 64 `RestirHoldEntry`: world
  centre (the event centre plus the ray scene anchor, so an anchor move does not shift a held sphere),
  reach and age. Ages stay on the CPU; the GPU gets anchor-relative centres and reaches.
- `restir_hold_update` runs once per frame after the event plan. Every entry ages by 1 and expires at
  `age >= spatial_hold_frames`, so a sphere is held on its event frame and the following
  `spatial_hold_frames - 1` frames. Each first fire (the same `since_last >= quiet_frames` test) either
  refreshes the first entry whose reach is at least its own and whose sphere contains its centre (age back
  to 0), or takes a free slot. A full ring replaces its oldest entry (highest age, lowest index on a tie)
  and counts `hold_dropped`. When the previous and current state of one light change share a centre and
  reach, the second event refreshes the first.
- The ring is cleared when the hold is off, the event path is off, the frame has no history (a cut, no
  ready ray scene, or the history test option off), or the frame is a jump frame; with the hold on, the
  frame's own first fires are then added (`restir_gi.cpp:621-630`). `restir_hold_update` with `hold_frames` 0
  also clears it, but the setting cannot be 0.
- `restir_hold_pack` writes `held[i]` = (centre - anchor, reach) and `hold[0]` = count. F_SPATIAL_HOLD
  (bit 25) is set when the hold is on and the ring is not empty. `rs_hold_caps`
  (`restir_gi_inc.glsl`) is the reach-scale and solid-angle test over held spheres, with no first-fire
  check.
- SPATIAL uses both the event test and the hold test, on the event frame and after it. The event test
  stays in addition to the hold test: a first fire that refreshed a larger held sphere adds no ring entry.
  TEMPORAL and VALIDATE do not read the held spheres. With the hold off or an empty ring, F_SPATIAL_HOLD is
  clear and `hold[0]` is 0, so SPATIAL uses only the event test.
- Status always (`restir_gi.cpp`): `spatial_hold`, `spatial_hold_frames`, `hold_fill` (entries after this
  frame's update), `hold_added`, `hold_refreshed`, `hold_expired` and `hold_dropped` (this frame, the last
  view to run). Backend-lifetime: `hold_frames_total` (view-frames with a non-empty ring),
  `hold_dropped_total` and `hold_fill_max`, summed or maxed over the views.
- Tests in `modules/gi_restir/tests/test_restir_events.h` cover: a sphere held for exactly
  `spatial_hold_frames`; refresh by a contained sphere; first fires only; dropping the oldest in a full
  ring; clearing and the age-tie rule; world positions across an anchor move; and `restir_hold_caps`
  matching the event test.

## B2k, keep M (gi/mode B2k)

`modules/gi_restir/restir_gi_core.cpp` sets the mode bits and
`modules/gi_restir/shaders/restir_gi.glsl` and `modules/gi_restir/shaders/restir_gi_inc.glsl` implement
keep-M.

- `gi/mode` is B1, B2, B2k or U1 (default B2). B2k is `RESTIR_MODE_B2K` (3). `_upload` writes
  `mode_code = mode | (F_FRESH_LO set ? 4 : 0)`, so B2k reads 3 or 7; only debug view 63 reads it.
- B2k is B2's flags, F_VIS_REUSE included, plus F_KEEP_M (bit 22). `_upload` sets it from the mode (see Fold,
  `_upload`, `modules/gi_restir/restir_gi_core.cpp:563`).
- A `Cand` has a `keep` bit. `rs_combine` adds M to the total when a candidate is on or kept; the weight
  loop skips anything not on, so a kept candidate contributes M with zero weight. This biases the result
  low, never bright.
- Kept: `rs_fill_neighbour`'s early return when the dead reason is REJ_VIS or REJ_DIRTY, which covers
  TEMPORAL's visibility-failed history state and its dirty history; and the receiver visibility test that
  turns a B-mode candidate off. Under keep-M, TEMPORAL's dirty test of a valid stored sample marks it
  unusable with REJ_DIRTY instead of rejecting the history, so its M, after `m_temporal_cap` and the event
  cap, stays in.
- Not kept: shift-limit failures (distance, cosine and Jacobian from `rs_shift_jacobian`); an invalid
  stored sample (RF_VALID clear, dead reason REJ_NO_SAMPLE, dirty or not); and candidates that never enter
  (out of frame, not similar, no stored sample, epoch). An invalid stored sample must not be kept: its M
  would ride an empty reservoir from frame to frame and never decay.
- U1 is untouched: F_KEEP_M is never set with F_UNBIASED, and U1's total already counts every present candidate. With
  B1, B2 or U1, `keep` stays false (dirty history per mode: see "Dirty test"). No test covers the keep rule; it lives
  in the shader.

## Sky re-read (gi/sky_reread, default off)

`modules/gi_restir/restir_gi.cpp` decides the effective flag; `modules/gi_restir/shaders/restir_gi.glsl`
does the re-read and `modules/gi_restir/shaders/restir_gi_inc.glsl` declares its bindings.

- F_SKY_REREAD is flags bit 26. `_upload` (`restir_gi_core.cpp`) sets it from `RestirFrameParams::sky_reread` alone
  (see Fold, `_upload`, `modules/gi_restir/restir_gi_core.cpp:563`). `resample` sets the parameter only when
  `gi/sky_reread` (default false) is on, a ray scene is ready, the shading inputs and DDGI UBO are valid, and the ray
  variant built its sky modes (`RestirGiCore::is_sky_valid`). `run_frame` repeats the test (VARIANT_NONE, no DDGI UBO,
  `sky_valid` false), so a run without rays never runs the sky path. With the own sampler and the constant test sky
  (F_REAL_SKY clear) the live sky is the test sky.
- Under the flag, a sample with RF_SKY takes its radiance, and therefore `rs_phat`, from
  `rs_sky_radiance(normalize(x_s))`. TEMPORAL uses it for the fresh sky candidate and, in
  `rs_fill_neighbour`, for a history sky sample after the unusable-sample return. SPATIAL uses it for the
  pixel's own valid sample with M > 0 and for each usable neighbour. The value is cached in
  `Reservoir.lo`, so `rs_phat` is unchanged.
- In the TEMPORAL and SPATIAL sky modes `rs_store` writes Lo 0 for every RF_SKY sample. FINAL takes a sky
  sample's radiance from the live sky before the visibility block and then skips its fresh_lo sky lookup,
  so there is still one lookup. In the VALIDATE path that runs without `gi/replay_seed` and without events,
  with the flag set, a sky sample whose visibility ray lands keeps its stored Lo (zero) and M with no
  change cut; a blocked sky sample loses M as before. The replay and event VALIDATE paths already reject
  sky samples.
- `rs_sky_radiance` (`shaders/restir_gi.glsl:73-98`) returns min(`test_sky`, `RS_LO_CLAMP`) when `F_REAL_SKY` is clear.
  Otherwise `rf.shade_sky_flags` decides: `RS_SKY_SKY` (2) rotates the direction by the quaternion (`shade_sky.xyz`, w
  = plus or minus sqrt(1 - |xyz|^2), the sign from `RS_SKY_SIGN` 4) and reads `sky_radiance` with `textureLod` at lod
  `rf.sky_blur_lod` (default 2) and `vec3_to_oct_with_border`, times `shade_sky_energy`; `RS_SKY_COLOR` (1) returns `shade_sky *
  shade_sky_energy`; neither gives 0; the result is clamped to `RS_LO_CLAMP`. The fields come from `RtgiShadeInputs`
  (`sky_color_or_orientation`, `sky_flags`, `sky_border_size`, `sky_energy`), copied in `resample` only when `shade`
  is valid (`modules/gi_restir/restir_gi.cpp:654-665`).
- Six sky modes exist in `restir_gi.glsl`: PASS_TEMPORAL_SKY, PASS_SPATIAL_SKY, PASS_SPATIAL_COMPAT_SKY,
  PASS_SPATIAL_WIDE_SKY and PASS_SPATIAL_COMPAT_WIDE_SKY (the plain mode's defines plus SKY_REREAD), and
  PASS_SKY_EPOCH (MODE_SKY_EPOCH). PassMode values are 11 to 16. With RESTIR_RAYS and SKY_REREAD, the
  TEMPORAL and SPATIAL sky modes define RS_SKY_REREAD.
- The sky modes use a second `RestirGiShaderRD` per variant (`sky_shaders`, `sky_versions`); `_compile`
  (`restir_gi_core.cpp`) puts them in the same pipelines table at `PASS_SKY_FIRST + i`. `valid[]` covers
  the 11 plain modes and `sky_valid[]` the six sky modes. `_compile` frees every pipeline of a set and its
  version when any mode fails, so the six are all or nothing. A failure leaves the variant running with the
  flag off and status `sky_reread` reports "unavailable". VARIANT_NONE does not build the sky shader;
  `_ensure` builds both for a ray variant on its first use.
- Set 2 (`restir_gi.glsl`): RESTIR_SHADE (FINAL, SOURCE, VALIDATE) and RS_SKY_REREAD share one block that
  defines `DDGI_SET` 2, `DDGI_UBO_BINDING` 0, the DDGI sampling bindings 3 to 6 (RESTIR_SHADE only),
  binding 7 `sky_radiance` (texture2DArray under USE_RADIANCE_OCTMAP_ARRAY, texture2D otherwise) and
  binding 8 `sky_sampler`, and includes `restir_core_ddgi_inc.glsl` exactly once. The sky modes need that
  include for the DDGI UBO at set 2 binding 0 and for `vec3_to_oct_with_border`. `_dispatch` binds set 2
  for the sky modes through `_shade_set`, as for FINAL, so TEMPORAL, SPATIAL and FINAL of one frame use
  the same sky texture and sampler. `run_frame` chooses the sky twin of TEMPORAL and of the SPATIAL mode it
  would run (`_sky_twin`).
- The sky epoch is a 32-byte storage buffer (`RestirSkyEpoch` in `restir_events.h`: luma, epochs, jump,
  jumps, capped and three spare uints; `static_assert` on its size). It is created zeroed on the first
  frame with the flag on (alloc log "sky_epoch"), survives frames and cuts, and is freed with the view's
  resources. A trace-size change reallocates the state and the epoch starts over.
- PASS_SKY_EPOCH runs one 8x8 group (64 invocations) in the frame's ray variant after VALIDATE and before
  TEMPORAL. It binds set 0 only: binding 0 the frame UBO, 26 the event UBO, 29 the epoch buffer, 30 the sky
  texture and 31 a linear mipmapped sampler. MODE_SKY_EPOCH undefines RESTIR_RAYS, so it declares no set 1
  or 2 and `_dispatch` binds neither.
- The mean luma (`restir_gi.glsl`, MODE_SKY_EPOCH): with F_REAL_SKY clear, `luma(min(test_sky, RS_LO_CLAMP))`. With a sky texture, the mean over the lowest mip (`textureQueryLevels - 1`, layer 0 in
  the array form) of `luma(min(texel * sky_energy, RS_LO_CLAMP))`; a mip larger than 64 texels on either axis is
  strided to at most 64 rows of at most 64 texels. Invocation i sums row i into a shared slot with its
  texel count, then a fixed tree (halves 32 down to 1, a barrier after each) adds slot i + half into slot
  i; invocation 0 divides. The order of every addition is fixed. The texels are read directly, so the
  sky's rotation does not change the mean. With a constant colour, `luma(min(colour * energy, RS_LO_CLAMP))`;
  otherwise 0. This mip is not the one `rs_sky_radiance` samples (`textureLod` at `rf.sky_blur_lod`), so the two differ
  in absolute luma; the epoch rule compares ratios only.
- `restir_sky_epoch_step` (`restir_events.cpp`) is the CPU statement of the rule that invocation 0 of
  MODE_SKY_EPOCH mirrors. It clears the jump word, ignores a NaN or infinite luma, and when there is no
  epoch yet or no history this frame (F_HISTORY clear: first frame, a cut, or the history test off) stores
  the luma and increments epochs without a jump. Otherwise it jumps when the luma is below 0.8 or above
  1.25 times the stored epoch luma (`RESTIR_SKY_EPOCH_LOW`, `RESTIR_SKY_EPOCH_HIGH`, the defaults of
  `gi/sky_epoch_low` and `gi/sky_epoch_high`, which the shader reads from `rf`), stores the luma, and
  increments epochs and jumps; it also increments capped when the jump caps. The comparisons multiply, so
  a luma exactly at either threshold is inside and an epoch at luma 0 jumps on any positive luma. epochs counts the first frame,
  every frame with F_HISTORY clear and every jump. The epoch anchor is the luma of the last epoch, not a
  running mean; it moves only at a jump or a cut.
- TEMPORAL's sky mode reads binding 29 (readonly) and treats the jump word like the CPU jump: in
  `rs_event_cap_history`, after the `gates[2]` cap, it does `m = min(m, jump_m_cap)` when the word is set. That
  function runs only under F_EVENTS, and `jump_m_cap` is in the event UBO, so a sky jump caps M only with
  `gi/event_reshade` on. With events off the epoch is still followed and counted, and nothing is capped.
- `capped` counts jumps that lowered the cap: the epoch pass adds 1 when F_EVENTS is set, the CPU jump
  word (`gates[2]`) is clear and `jump_m_cap` is below `m_temporal_cap`. `restir_sky_jump_caps` is the
  tested CPU mirror. It counts frames where the cap applied, not pixels whose M fell.
- The CPU jump frame comes from the event plan, which ignores the sky epoch jump, so the CPU does not
  select VALIDATE ALL and does not clear the hold for one; status `jump_frame` stays the CPU jump.
- One sky per frame: `run_frame` never re-reads the sky. The epoch pass, TEMPORAL, SPATIAL and FINAL all
  bind the RID from `RestirGiInputs`, or the white default.
- Switching is a history cut. `resample` sets the epoch planner's `hardware_changed` when the hardware flag
  or the effective flag differs from the last frame's (`RestirGiState::sky_reread`, `restir_gi.cpp:464`);
  the cut is filed under RESTIR_CUT_BACKEND (a first frame or a requested cut takes precedence,
  `restir_epochs.cpp`), and `sky_reread_cuts` counts the backend cuts that had the hardware unchanged. The
  cut clears F_HISTORY for the frame, so the epoch pass starts a new epoch at the current luma when the
  flag returns. Without the cut, a sky sample stored under the flag has Lo 0, and its M would still enter
  the combine and darken the result.
- Cost: one `textureLod` at `rf.sky_blur_lod` per sky technique evaluated in TEMPORAL and SPATIAL and one per sky pixel
  with W > 0 in FINAL; one 64-invocation epoch pass; 32 bytes of buffer.
- While the setting stays off, status is "off", bit 26 is clear, no epoch pass runs, no epoch buffer is made
  (one made earlier stays allocated) and the effective flag causes no cut; TEMPORAL and SPATIAL run the
  plain modes. The sky shader is built for a ray variant but not dispatched. FINAL and VALIDATE test bit 26
  at run time and, with it clear, take their plain path.
- Status (`restir_gi.cpp`): `sky_reread` is "off" (setting off), "inactive" (setting on but no ray scene,
  shading inputs or DDGI UBO), "unavailable" (the sky modes failed to build) or "on"; it reflects the last
  view to run. `sky_epochs`, `sky_jump_frames_total` and `sky_jump_capped_total` are the epoch buffer's
  epochs, jumps and capped summed over views; `sky_reread_cuts_total` sums the cuts the flag caused. They
  count over a view state's life.
- The epoch counters are cached. `get_status` is const and never reads the GPU; `resample` refreshes a
  view's copy (`read_sky_epoch`) after `read_stats` on frames with `options.readback`, so the 32-byte read
  adds no stall. `sky_epoch_source` is always "cached"; `sky_epoch_frame` is the newest refresh frame index
  across views, or -1; the three totals stay 0 for a view until it refreshes.
- Tests in `modules/gi_restir/tests/test_restir_events.h` cover: the epoch starting on the first frame
  without a jump; luma inside the band with the edges included; jumps outside it; each jump measured from
  the new epoch; a cut restarting the epoch without a jump; a black sky jumping on any light and a
  non-finite luma being skipped; and the capped condition (`restir_sky_jump_caps`).

## Traced tiles only and M in PACK (gi/trace_all_tiles off, gi/confidence)

The module side is in `modules/gi_restir/restir_gi.cpp`, `restir_events.cpp`, `restir_gi_core.cpp`,
`shaders/restir_gi.glsl` and `shaders/restir_gi_inc.glsl`.

### Flags and the tile texture
Source: `modules/gi_restir/restir_gi_core.h`, `restir_gi_core.cpp` (`RestirGiCore::_upload`), `restir_gi.cpp`,
`shaders/restir_gi_inc.glsl`.
- `RESTIR_F_TILE_SKIP` is bit 27 (134217728) and `RESTIR_F_PACK_M` bit 28 (268435456); the include repeats them as
  `F_TILE_SKIP` and `F_PACK_M`. Bit 19 (524288, `RESTIR_F_PRE_FRAME`) is Fold's pre-frame flag.
- `_upload` rebuilds bits 22, 26, 27 and 28 from the frame params and masks them out of the caller's flags (see Fold,
  `restir_fold_words`; `modules/gi_restir/restir_gi_core.cpp:563`), so a harness capture cannot set them.
- The tile texture is R2's `RB_RTGI_TILES` (scope `RB_SCOPE_RTGI`, `rtgi.cpp`): R32_UINT, sampling and storage usage,
  one texel per 8x8 tile (`tile_count = ceil(trace_size / TILE_SIZE)`). CLASSIFY writes it as a storage image.
- The module declares it as `layout(set = 0, binding = 27) uniform utexture2D rs_tiles`, only for `MODE_TEMPORAL`,
  `MODE_SPATIAL` (shared by the SPATIAL modes and their sky twins), `MODE_FINAL` and `MODE_PACK`. CLEAR, VALIDATE,
  SOURCE, CAPTURE and SKY_EPOCH neither declare nor read it.
- The RID arrives through `RtgiFrameInputs::tile_texture` (`gi_module_hooks.h`); `rtgi.cpp` fills it with the RID it
  binds to CLASSIFY (binding 6, storage image) and the trace (binding 3). `restir_gi_core.cpp` binds it at set 0
  binding 27 for TEMPORAL, SPATIAL, FINAL and PACK (`restir_gi_core.cpp:934,983,1007,1026`). `restir_gi.cpp:704`
  passes it to the core only when `params.tile_skip` is set.
- A tile is traced when bit 0 (`RTGI_TILE_TRACE`) of its texel is set. `ddgi_trace.glsl` (RTGI mode) treats a pixel as
  a trace candidate when that bit is set and the pixel has depth.
- `RtgiPass` forces every tile to trace (`settings.force_trace`) when the backend's `wants_all_tiles()` is true, which
  is `gi/trace_all_tiles` (`RestirGiBackend::wants_all_tiles`; the hook default is true). Off lets RTGI choose its
  tiles; the module stores empty reservoirs in the untraced ones and passes no output there, so DDGI, surfels or SSGI
  show there.

### `rs_tile_traced` and the dummy texture
Source: `modules/gi_restir/shaders/restir_gi_inc.glsl` (`rs_tile_traced`), `restir_gi_core.cpp`
(`RestirGiCore::_tile_texture`), `restir_gi.cpp`.
- It returns true without a read while `F_TILE_SKIP` is clear. Otherwise it reads
  `tile = min(p_pixel / RTGI_TILE, textureSize(rs_tiles, 0) - 1)` and tests `RTGI_TILE_TRACE`.
- `RTGI_TILE` is 8 and passes run in 8x8 groups, so every invocation of a group reads the same texel and the skip is
  uniform per group. `rs_stats_store`'s barrier and FINAL's barrier are reached by every invocation, so a skipped
  group does not diverge there. `restir_gi.cpp` static_asserts `RESTIR_TILE_SIZE == RtgiPass::TILE_SIZE`.
- With no texture in the inputs, `_tile_texture` lazily creates a 1x1 R32_UINT holding `RESTIR_TILE_TRACE` (sampling
  and update usage); `free()` releases it. The clamp makes a read of it mean "traced" everywhere, so bit 27 with no
  texture runs as if every tile were traced.
- The frozen-frame PACK is described under "Test options" (`modules/gi_restir/restir_gi.cpp:425-449`).

### The tile plan
Source: `modules/gi_restir/restir_events.cpp` (`restir_plan_tiles`, `restir_tile_state_name`), `restir_gi.cpp`,
`restir_gi_core.cpp` (`RestirGiCore::run_frame`).
- `trace_all` true: `RESTIR_TILES_ALL` ("all"); nothing is read.
- false, with a texture of `ceil(trace/8)` texels in both axes: `RESTIR_TILES_SKIP` ("skip").
- false, with no texture, a zero trace axis, or any other texture size: `RESTIR_TILES_UNAVAILABLE` ("unavailable").
  The module then covers every tile while R2 traces only its own tiles.
- `restir_gi.cpp:565` sets `params.tile_skip` only for SKIP; `run_frame` (`restir_gi_core.cpp:861`) clears it if the
  inputs carry no texture. The harness (`restir_harness.cpp:358`) passes none.

### What an untraced group does
Source: `modules/gi_restir/shaders/restir_gi.glsl`.
- TEMPORAL: no candidate read, no history read, no rays; it stores `rs_empty()` and info 0.
- Dump and lineage variants: `rs_fold_dump_scan(px)` runs before the tile test, so its scan counts empty untraced
  texels too, and the lineage untraced branch calls `rs_fold_lineage_store(px, px, false, false, false)`; see "Fold
  dump and self-test" and "Fold lineage" (`modules/gi_restir/shaders/restir_gi.glsl:664-682`).
- SPATIAL, every mode: no taps, no rays; it stores `rs_empty()` and leaves info untouched (TEMPORAL wrote 0 this
  frame). It does not write `sel_image`.
- FINAL: vis_state 0, radiance (0, 0, 0, 1), coverage 0, debug 0 under `F_DEBUG_OUT`, info untouched. The group's
  stats row is written and is all zero, so it adds nothing to the host's reduction.
- PACK: `NO_CANDIDATE | NOT_TRACED`, radiance 0, pdf word 0, tag 0. R2's TEMPORAL then zeroes its history there:
  `NO_CANDIDATE` without `THINNED` means no carry (`rtgi.glsl:363-368`). Where no tap has history, R2's COMPOSE
  writes coverage 0 (`rtgi.glsl:713-716`), and `gi_compositor_blend` returns the lower source at coverage 0.
- TEMPORAL and SPATIAL clear their `g_stats` first and call `rs_stats_store` outside the tile branch, so an untraced
  group's row is all zero (see "Statistics").
- VALIDATE, CLEAR, SOURCE, CAPTURE and SKY_EPOCH do not read the texture. VALIDATE edits the previous history in
  place, and its `m == 0` early-out skips the empty reservoirs an untraced tile left there. SOURCE has no tile test
  (it traces every pixel with a valid receiver); TEMPORAL ignores those candidates in untraced groups.

### Taps into untraced tiles
Source: `modules/gi_restir/shaders/restir_gi.glsl` (`rs_spatial_pixel`, `rs_select_neighbours`, `rs_median_w`).
- The offsets path (no `PASS_SPATIAL_COMPAT`) tests the tap's tile after the frame test and before `rs_receiver_cur`,
  and records `REJ_UNTRACED` (14) in the spatial reject nibble; `REJ_BOUND` is 13.
- The compatibility selection (`rs_select_neighbours`) skips such a tap before scoring, like an out-of-frame tap, so
  the pick slot goes to a traced tap. It records no reason, so a receiver whose taps all fall in untraced tiles reads
  `REJ_NO_COMPAT` (12); 14 appears on the offsets path only.
- The tapped reservoir is empty in any case, so the gate only saves fetches and pick slots; the selection scores
  receivers from geometry and checks tile flags, and reads no reservoir. FINAL's 3x3 median of the reservoir weight
  `w` counts only neighbours with `m > 0`, `w > 0` and `w` not infinite.

### Re-trace, cuts and readback
Source: `modules/gi_restir/restir_gi.cpp`, `restir_gi_core.cpp`.
- A tile that is traced after being skipped starts from empty reservoirs: a history tap that lands in a tile skipped
  on the previous frame reads `m == 0` (`REJ_NO_SAMPLE`) and vis_state there is 0 (FINAL wrote it), so its
  visibility-fail count restarts at 0; spatial reuse from traced neighbours fills it.
- Neither `gi/trace_all_tiles` nor `gi/confidence` takes part in the history-cut plan, so changing either does not cut
  history. Turning the skip on empties the untraced tiles that same frame; turning it off lets them fill from the
  candidates.
- FINAL rows of untraced groups are 0, so with readback on, `geometry`, `output_pixels`, `m_sum` cover traced
  pixels only.

### M in PACK and `gi/confidence`
Source: `modules/gi_restir/restir_gi.cpp`, `shaders/restir_gi.glsl` (`MODE_PACK`), `restir_events.cpp`
(`restir_pack_word`), `restir_gi_core.cpp` (`run_pack`).
- `gi/confidence` is a string, `frames` (default) or `frames_and_m`. `params.pack_m` is true only for `frames_and_m`,
  whatever `gi/trace_all_tiles` says.
- `frames`: `F_PACK_M` stays clear and PACK's pdf word stays 0. Coverage comes from R2's history count alone
  (n̄ / T, T = `target_samples`).
- `frames_and_m`: `F_PACK_M` is set. PACK reads the final reservoir set (binding 7, `reservoir[final_index]`, bound by
  `run_pack`) and, where it packs a candidate (flags 0: a covered pixel, or a blocked one on the `INFO_VIS_BLOCKED`
  path), writes `float(M)`, the low 16 bits of layer 1's `.w`, into the pdf word. This happens in traced tiles only;
  THINNED and NO_CANDIDATE pixels keep 0.
- R2 consumes the word: TEMPORAL mixes `min(M / n_target, 1)` into `moments.w` and COMPOSE uses it for route coverage
  (next section).
- PACK runs only when `gi/denoise` is `r2` (`restir_gi.cpp:737-740`; the frozen path at 429-434 as well).
- `restir_pack_word` mirrors PACK's decisions on the CPU. Test cases "[RestirEvents] Tile plan ..." and
  "[RestirEvents] Pack word ..." in `modules/gi_restir/tests/test_restir_events.h` cover the plan and the word.

### Status and the defaults
Source: `RestirGiBackend::status` in `modules/gi_restir/restir_gi.cpp`.
- Keys: `tiles` ("all", "skip" or "unavailable", from the last frame run), `tile_skip_frames_total` (frames run with
  the skip, summed over every view the backend runs) and `confidence` ("frames" or "frames_and_m").
- With the defaults (`gi/trace_all_tiles` on, `gi/confidence` frames), bits 27 and 28 are clear, `rs_tile_traced`
  returns true without a read, every pass takes its traced path and PACK's pdf argument is 0.0.

## R2 side: module output, route coverage, tile memory

The core side is `servers/rendering/renderer_rd/environment/rtgi.cpp`, `rtgi.h`,
`servers/rendering/renderer_rd/shaders/environment/rtgi.glsl` and `rtgi_inc.glsl`; the module side is
`restir_route_output` in `modules/gi_restir/restir_events.cpp` and its use in `modules/gi_restir/restir_gi.cpp`. No
`rtgi.*` file names the module; the decision crosses through the hook.

### Hook fields and when module output is on
Source: `servers/rendering/renderer_rd/environment/gi_module_hooks.h` (`RtgiResampleOutput`),
`modules/gi_restir/restir_events.cpp` (`restir_route_output`), `modules/gi_restir/restir_gi.cpp`.
- `RtgiResampleOutput` fields are described under "Backend contracts"; `history_cap` is T_r (`gi/denoise_history_cap`
  clamped 1..255, or the ramped cap).
- `restir_route_output(denoise, events_on, pack_m, n_target)` returns module output only when `denoise` (gi/denoise is
  r2) and `events_on` or `pack_m`. `events_on` is `gi/event_reshade` with `gi/replay_seed`. `n_target` is handed over
  only with `pack_m`, clamped to 1..255; the caller passes `gi/n_target` clamped to 1..64, so 255 is never reached.
- Defaults (events off, frames): plain. Flag 256 stays clear and R2 takes its ordinary restart branches.
- Events on, frames: flag 256 set, `n_target` 0. TEMPORAL skips R2's restarts; `moments.w` stays 0 and COMPOSE keeps
  `clamp(n̄ / T)`.
- `frames_and_m`, with or without events: flag 256 set and `n_target` handed over (route form). The restart skip holds
  with events off too: in route form R2's history count feeds coverage, and the restart-sphere cap would lower that
  count to `soft_cut_count`. With events off a light change acts through the Light epoch cap (see "Invalidation and R2
  integration"), which lowers M and so the M term of coverage.
- The frozen-frame test option reuses the last real frame's route; see "Test options"
  (`modules/gi_restir/restir_gi.cpp:436`).

### Flag 256 in core and TEMPORAL
Source: `rtgi.h` (`FRAME_FLAG_MODULE_OUTPUT`), `rtgi.cpp` (`RtgiPass`), `rtgi.glsl` (`MODE_TEMPORAL`).
- `FRAME_FLAG_MODULE_OUTPUT` is 256 (`RTGI_FRAME_FLAG_MODULE_OUTPUT` in `rtgi_inc.glsl`). `RtgiPass` sets it only when
  the backend's output is used (`denoise` valid) and the mode is `RTGI_OUTPUT_MODULE`; it sets it after the backend
  runs, so CLASSIFY and the trace never see it. The frame UBO is re-uploaded before TEMPORAL whenever the frame is a
  denoise frame or a module cut. Under the flag, in TEMPORAL:
  - The early return for a pixel without geometry, or without a finite candidate and not carried, is unchanged and
    writes `moments.w` 0.
  - The gather also accumulates `moments_previous.w` with the same bilinear weights as `moments.x` and `.y`.
  - The global soft cut and the restart-sphere cap are skipped. `FRAME_FLAG_RESTART_SPHERES` and the sphere list are
    still set and uploaded, so `RTGI_COUNTER_RESTARTED` reads 0 on such a frame.
  - The carry store writes the gathered `.w`. The normal store writes `mix(w_previous, min(M / n_target, 1), blend)`
    with the same blend as the other moments (the bare sample when there is no previous count), M being
    `uintBitsToFloat(candidate_second.z)`. With `n_target` 0, or a word not above 0 (NaN included), the sample is 0.
- The CPU soft-cut bookkeeping in `rtgi.cpp` (`frames_since_cut` held at `soft_cut_count`) does not depend on the
  flag.

### Route form and marked tiles
Source: `rtgi.cpp` (`RtgiPass`), `rtgi.glsl` (`MODE_COMPOSE`, `rtgi_route_confidence`).
- `RtgiPass` writes T_r (`settings.history_cap`; see "Backend contracts",
  `servers/rendering/renderer_rd/environment/rtgi.cpp:676-678`) into `FrameUBO.target_samples` and `n_target` into the
  UBO word, only when flag 256 is set and `n_target` is above 0. Plain R2 keeps `target_samples`. The published R2
  record's confidence still divides by `rtgi/target_samples`, and its sample count is `min(frames_since_cut,
  history_cap)`.
- COMPOSE replaces `clamp(n̄ / T)` with `min(clamp(n̄ / T_r), clamp(μ̄ / N))`, where μ̄ is the bilateral mean of
  `moments.w` over the same taps and weights as n̄ and N is 1. Validity and support multiply as before, giving
  `coverage = confidence * validity * support`.
- COMPOSE reads the tile texture at binding 10 only in this route path, when `ddgi_wrong_bits` has ACTIVE and n̄ ≥ 2.
  It uses the tile of `round(tap_position)` clamped to the trace, that is the trace pixel nearest the slot pixel.
- A marked tile (`RTGI_TILE_REASON_DDGI_WRONG`, tile bit 64) with n̄ ≥ 2 and a relative deviation
  `sqrt(m2 − m1²) / max(m1, L_floor)` below `rtgi/ddgi_wrong_noise` uses T_r = 1 and
  `N = clamp(rtgi/ddgi_wrong_target / n_target, 1e-3, 1)`, where m1 and m2 are the bilateral means of `moments.x` and
  `.y` over the same taps. N is at most 1, so it can only raise the confidence term.

### Tile memory: textures, bits and reading
Source: `rtgi.cpp`, `rtgi.h`, `rtgi_inc.glsl`, `rtgi.glsl` (`MODE_CLASSIFY`, `rtgi_previous_trace_pixel`).
- Two R16_UINT textures, `tile_memory_0` and `tile_memory_1`, one texel per tile, created with R2's other textures and
  cleared at creation. They ping-pong with R2's other history pairs: CLASSIFY reads `tile_memory_previous` (binding 7)
  and writes `tile_memory_current` (binding 8). R2 allocates both (2 × tiles × 2 B) whatever the settings, because
  CLASSIFY declares them.
- Word layout: low 8 bits frames left (`RTGI_MEMORY_FRAMES`, 255); bit 8 (256, `RTGI_MEMORY_EVENTS`) set by
  `rtgi/ddgi_wrong_events`; bit 9 (512, `RTGI_MEMORY_DISAGREE`) set by `rtgi/ddgi_wrong_memory`.
- `FrameUBO.ddgi_wrong_bits`: EVENTS 1, MEMORY 2, ACTIVE 4, READ 8. ACTIVE means events or memory is on. READ needs no
  cut this frame (`CUT_NONE`) and `tile_memory_written` from the previous frame. EVENTS needs events on and a selected
  sphere count above 0 with no dropped sphere. MEMORY needs memory on and no cut.
- A refused or skipped frame leaves `tile_memory_written` false, so the next frame has READ clear and never reads a
  stale texture. A resize recreates and clears both textures; a change of `rtgi/resolution_scale` changes
  `trace_size`, which counts as a resize.
- `CUT_MODULE` is decided after these bits are computed, so on a module-cut frame the memory is still read.
- Reprojection: one trace pixel per tile reads, the centre (local 4, 4) or on a cut-off edge tile the in-range pixel
  nearest it, and only if it has geometry. It projects the view position to the previous clip and requires the point
  in front of the previous camera, on the trace, geometry at the rounded previous trace pixel, and a plane distance to
  that pixel within TEMPORAL's depth tolerance. The plane normal is the current normal turned into the previous view,
  because CLASSIFY binds no previous normal. The previous memory is read at that pixel's tile. The memory path's
  per-pixel history reads use the same test.
- Limits: a tile whose reading pixel has no geometry reads nothing, so its stored word counts as 0. Switching
  `gi/confidence` or events on a running frame is not a cut, so `moments.w` starts at 0 and rises through the temporal
  blend. With `gi/confidence_ramp` on, T_r is the ramped cap (`_ramped_cap` in `restir_gi.cpp`). The noise gate's m1
  and m2 are bilateral means, not the centre pixel's moments.

### Tile memory: per-frame update and the disagreement test
Source: `rtgi.glsl` (`rtgi_tile_memory`, `rtgi_tile_disagrees`, `MODE_CLASSIFY`), `rtgi.cpp`,
`rtgi_restarts.h` (`MAX_SPHERES`).
- left = stored − 1 (0 stays 0); marks clear when left reaches 0.
- `rtgi/ddgi_wrong_events` sets left = `rtgi/ddgi_wrong_frames` and bit 8 when any trace pixel with geometry in the
  tile lies inside one of the frame's view-space restart spheres.
- `rtgi/ddgi_wrong_memory` sets left and bit 9 on a tile carrying a DDGI, SSGI, check or forced reason, or having left
  above 0 after this frame's update, when its test passes.
- A tile with left above 0 gets reason 64 and `RTGI_TILE_TRACE`, so it is traced on the marking frame and
  `rtgi/ddgi_wrong_frames − 1` frames after it. Tiles with no geometry write 0.
- Overflow: when the sphere selection dropped spheres (it holds at most `MAX_SPHERES` = 16), EVENTS stays clear that
  frame and `r5_overflow` counts it. The sphere list is uploaded for the events path even when R2's own sphere restart
  is off or the frame is a soft cut; TEMPORAL reads it only under `FRAME_FLAG_RESTART_SPHERES`, and not under flag
  256.
- The test: each trace pixel with geometry reprojects as above and reads last frame's history; if the history alpha
  (count) is above 0 and the radiance finite, it adds (luma, luma², 1) to a 64-entry shared array that thread 0 sums.
  m_R is the mean, var_R the sample variance (÷ (n − 1)), n_eff = n / (1 + ρ (n − 1)) and SE_R = sqrt(var_R / n_eff).
  m_D is the mean luma of the classify DDGI taps (local (2,2), (6,2), (2,6), (6,6)) that ran and have DDGI alpha above
  0 (`shared_ddgi` is vec4[4] and starts at alpha 1). Mark when n ≥ 2, at least one tap,
  |m_R − m_D| / max(m_D, L_floor) > τ and SE_R < τ m_R / 3.
- Neither mean carries an energy factor: `ddgi_sample` (`ddgi_inc.glsl`) does not apply `ddgi.energy`, history
  radiance is stored without `rtgi.energy`, and COMPOSE multiplies `rtgi.energy` into the output.

### Frame UBO and settings
Source: `FrameUBO` in `rtgi.h`; `GLOBAL_DEF_INTERNAL` in `servers/rendering/rendering_server.cpp`; clamps in
`rtgi.cpp`.
- `FrameUBO` has two 16-byte rows after `restart_spheres`: (ddgi_wrong_frames, ddgi_wrong_bits, n_target,
  ddgi_wrong_rho) at 624 and (ddgi_wrong_tau, _target, _noise, _floor) at 640. `sizeof(FrameUBO)` is 656 and four
  static_asserts pin `restart_spheres` at 368, the two rows at 624 and 640, and the size. The ddgi_wrong words stay 0
  unless events or memory is on, and n_target unless the route form runs.
- The settings are registered with `GLOBAL_DEF_INTERNAL`, so none is in ProjectSettings.xml; the ranges are the clamps
  in `rtgi.cpp`.

| Setting | Range | Default |
|---|---|---|
| rtgi/ddgi_wrong_events | bool (events) | false |
| rtgi/ddgi_wrong_memory | bool (memory) | false |
| rtgi/ddgi_wrong_frames | int 1..255 | 16 |
| rtgi/ddgi_wrong_tau | float 0..16 (τ) | 0.5 |
| rtgi/ddgi_wrong_target | float 0..255 (N_w) | 2.0 |
| rtgi/ddgi_wrong_noise | float 0..16 | 0.25 |
| rtgi/ddgi_wrong_floor | float 1e-6..1e6 (L_floor, luma) | 0.05 |
| rtgi/ddgi_wrong_rho | float 0..1 (ρ) | 1.0 |

With ρ 1, n_eff is 1.

### Status and identity
Source: `RtgiPass::_write_status` in `rtgi.cpp`; `RestirGiBackend::status` in `restir_gi.cpp`;
`modules/gi_restir/tests/test_restir_events.h`.
- R2 status: `ddgi_wrong {bits, r5_frames, r5_overflow}` and `route {module_output_frames, route_frames, n_target}`.
  With `rtgi/gpu_counters`, the counters `tiles_wrong_events` and `tiles_wrong_memory` count tiles whose memory word
  written this frame has bit 8 or 9.
- Module status: `tiles`, `tile_skip_frames_total`, `confidence`, `module_output`, `route_n_target`,
  `module_output_frames_total`; the stats dictionary carries `no_canonical`. There is no spikes key.
- With the module off, or on with the defaults, flag 256 is clear; with events and memory off the ddgi_wrong words are
  0 and CLASSIFY's memory code is inert. CLASSIFY then takes reasons from geometry, DDGI alpha, SSGI validity, check
  and force only, and writes the same geo and tile texels. TEMPORAL takes both restart branches and stores `moments.w`
  0. COMPOSE keeps `clamp(n̄ / T)` and reads no tile. VARIANCE and ATROUS read neither the ddgi_wrong words nor the
  tile memory; DEBUG reads the tile texture for its existing views.
- Test cases "[RestirEvents] Route output ..." cover `restir_route_output`: plain with the defaults and without the r2
  route, module output under events or M, and the 1..255 clamp on n_target.

## Fold, the stored-age bound (gi/fold_k, default 0)

The setting is `rendering/global_illumination/restir/gi/fold_k` (`modules/gi_restir/restir_settings.cpp`: integer,
range 0..64, step 1, default 0); `RestirGiBackend::resample` reads it. The pure rules live in
`modules/gi_restir/restir_fold.cpp`. `restir_fold_k` returns 0 for 0 or less; otherwise it clamps to 64 and rounds
down to a power of two, with 4 as the least value (1 to 7 read 4, 8 to 15 read 8, 63 reads 32, 64 or more read 64).

`restir_fold_k_eff` returns that K only when the mode is not U1 and spatial feedback is off, else 0.
`RestirGiBackend::resample` (`modules/gi_restir/restir_gi.cpp`) takes the mode from `gi/mode` (default B2) and
feedback from the spatial_feedback test option when it is set, else from the mode (U1 on, B modes off). It keeps
the rounded K, mode, feedback and `k_eff` in the state's `RestirFoldFrame` and passes `k_eff` in
`RestirFrameParams`.

`RestirGiCore::_upload` (`modules/gi_restir/restir_gi_core.cpp`) takes its flags, clock and K words from
`restir_fold_words` (`modules/gi_restir/restir_fold.cpp`). That function masks bit 19 out of the caller's flags,
re-runs `restir_fold_k_eff` on the supplied K with `mode == U1` and `spatial_feedback`, sets bit 19 again only
when the gated K is nonzero and the pre-frame is requested, and clears the clock word when the gated K is 0.
`_upload` then rebuilds bits 22 (KEEP_M), 26 (SKY_REREAD), 27 (TILE_SKIP) and 28 (PACK_M) from the frame params.
A harness dict without `k_eff`, or with mode U1 or `spatial_feedback` true (the `RestirFrameParams` default, so
also a dict without that key), therefore runs with Fold off.

### Storage and the history set — `modules/gi_restir/shaders/restir_gi_inc.glsl`

`s` is bits 8 to 13 of a reservoir's flags (`RF_S_SHIFT` 8, `RF_S_MASK` 0x3F); bit 14 is `RF_MV`, set when the sample
has moved from another pixel or began as a start. Bit 15 is free and `RF_FOLD_BITS` (0xFF00) names the byte. The flags
word and texture layout are under "Storage and frame buffers".

`rs_empty()` has flags 0, and `rs_from_candidate` starts from it and sets only `RF_VALID` and the sky, backface
and emissive bits. In MODE_TEMPORAL with Fold on, the Fold write after `rs_finish` is the only writer of bits 8
to 15.

MODE_VALIDATE (`restir_gi.glsl`) re-stores the flags in the high half of layer 1's `y` on every path that keeps the
sample, and clears them together with M only for a sample whose ray no longer lands, which then reads as a start. With
spatial feedback off the set routing is under "Pass chain" (`modules/gi_restir/restir_gi_core.cpp:1011`); only the
Fold write decides those bits there.

### TEMPORAL order and rule 1 — `modules/gi_restir/shaders/restir_gi.glsl`

In `rs_temporal_pixel`, `hp` is declared outside the `F_HISTORY` block and stays `ivec2(-1)` when no history
projection is computed. `h_s` and `h_mv` are read where the history is loaded, before rule 1 can clear it.

`had` is `c[1].present || cleared`. `rs_fill_neighbour` sets `present` at entry, so a history it later rejects
(unusable, a shift limit, a visibility failure, a dirty history) is still present and is a carry unless rule 1
clears it; the reject code does not decide the class. The bound rests on presence.

Rule 1 runs right after `rs_fill_neighbour`, only when `RS_FOLD_K` is nonzero. `rs_fold_refuses` is true when
`h_s + 1 >= RS_FOLD_K`, or `>= RS_FOLD_K - 1` on a pre-frame. A refusal sets `c[1] = rs_cand_none()` (so its M,
and a kept M, leave the total), sets `cleared`, adds one to `PS_FOLD_REFUSED`, and sets `REJ_BOUND` (13) only
when the reason is still `REJ_NONE`. `INFO_HAD_HISTORY` stays as the load set it.

### The write — `modules/gi_restir/shaders/restir_gi.glsl`

After `rs_finish`, with `RS_FOLD_K` nonzero, the shader writes
`result.flags = (result.flags & ~RF_FOLD_BITS) | rs_fold_bits(...)`. It replaces the byte because `rs_combine`
copied the selected technique's flags into it.

- carry (`had`, not `cleared`): `s = h_s + 1`, `mv = h_mv || hp != px`; `PS_FOLD_CARRIED`.
- renewal (`cleared`): `u = (v + phi(px)) mod K` when `h_mv` is 0 and `hp == px`, else `psi_r(px, v) mod K`;
  `s = u` when `u <= K/2`, else `u - K/2`; `mv = 0`; `PS_FOLD_RENEWED`.
- start (no history: `REJ_CUT`, `REJ_FRAME`, a similarity reject, `REJ_NO_SAMPLE`, `REJ_EPOCH`, or the biased
  `REJ_DIRTY`): `s = draw(psi_s(px, v))`, `mv = 1`; `PS_FOLD_STARTED`.

On a pre-frame every birth (renewal or start) is capped at `K - 2`; a renewal is at most `K/2`, so the cap binds
on starts. A carry cannot exceed the bound because a pre-frame refuses `h_s >= K - 2`.

The write runs for every valid traced receiver, including one whose stored reservoir is empty: a refusal on a
pixel with no fresh candidate (any pixel on a revalidation frame, or one ray thinning skipped) and a pixel with
neither candidate nor history both store M 0. The next frame reads M 0, takes the start branch and overwrites
those bits; the stored `s` is never carried.

### Draw and hashes — `modules/gi_restir/shaders/restir_fold_inc.glsl`

`rs_fold_draw` uses `quarter = K*K/4` and `x = psi_s mod 3*quarter`; it returns `rs_fold_isqrt(x)` when `x < quarter`,
else `K/2 + (x - quarter)/K`. Only `x` below `K*K/4` reaches `rs_fold_isqrt`, so its argument is at most 1023 at K 64.
`rs_fold_isqrt` starts from `uint(sqrt(float(x)))` and corrects once upward and once downward. The result is below K:
weight 1 at s = 0, 2s + 1 below K / 2, K from K / 2.

`rs_fold_phi(px) = rtgi_pcg(px.x + rtgi_pcg(px.y + RS_FOLD_C1))`; `psi_r` is `rtgi_rng_state(px, v, RS_FOLD_DIM_RENEW,
RS_FOLD_C2)` and `psi_s` is the same with `RS_FOLD_DIM_START` (0 and 1); the macros `RS_FOLD_PSI_R(pixel, v)` and
`RS_FOLD_PSI_S(pixel, v)` expand to them. `RS_FOLD_C1` is 0x68E31DA4 and `RS_FOLD_C2` is 0x2C1B3C6D. `rtgi_rng_state`
(`servers/rendering/renderer_rd/shaders/environment/rtgi_inc.glsl`) hashes `dimension + seed` first, so the two
streams are C2 and C2 + 1.

Other `rtgi_rng_state` callers and the doctest that keeps C2 and C2 + 1 at least 5 from their seed sums (default seed
1 and the `F_DETERMINISTIC` seed only) are under "Run counters", Seeds and CPU mirrors
(`modules/gi_restir/restir_fold_mirror.cpp:83-102`, `modules/gi_restir/tests/test_restir_fold_rows.h:59`); `resample` sets
`rf.seed` from `rendering/global_illumination/rtgi/seed` (default 1) plus the test seed offset.

### Clock, pre-frame and cuts — `modules/gi_restir/restir_gi.cpp`

`v` is `RestirGiState::frames`. `resample` copies it into `params.fold_clock` before `state.frames++` and puts
`k_eff` in `params.fold_k_eff`. `_upload` stores the gated words in `RestirFrameUBO::fold_clock` and
`fold_k_eff`, which the GLSL block names `shade_pad1` and `shade_pad2`
(`modules/gi_restir/shaders/restir_gi_inc.glsl`); MODE_TEMPORAL aliases them as `RS_FOLD_CLOCK` and `RS_FOLD_K`
(`restir_gi.glsl`).

The freeze path (see "Test options") returns before `frames++`, and `rtgi.cpp` calls `resample` only when
`begin_frame` returned true, so neither it nor a refused frame advances `v`. Reallocating the state, including after
`release`, resets `frames` to 0 and the first frame is `RESTIR_CUT_FIRST`. Because K is a power of two, the uint32
wrap of `v` leaves the phase continuous.

The pre-frame flag is `rf.flags` bit 19 (`F_PRE_FRAME`, `RESTIR_F_PRE_FRAME` 524288); `restir_fold.h` defines
`RESTIR_FOLD_F_PRE_FRAME` and `restir_gi_core.cpp` asserts the two agree.

The revalidation schedule is written once, as `restir_reval_frame(frame_index, period)` in
`modules/gi_restir/restir_fold.cpp`: false below period 2, else `frame_index % period == period - 1`. `resample`
calls it on `p_inputs.frame_index` to set `RESTIR_F_REVAL_FRAME`, only with history, a ready ray scene and
`gi/revalidate` set to budget.
`restir_fold_pre_frame(budget, rays, frame_index, period)` requires both booleans and returns
`restir_reval_frame(frame_index + 1, period)`, which equals `frame_index % period == period - 2`. At frame_index
0xFFFFFFFF the addition wraps to 0, so the predicate is false for every period 2 to 16. It does not read
`use_history`, so a pre-frame after a cut still caps births. `resample` sets it only with nonzero `k_eff`,
budget revalidation and a ready ray scene; `gi/revalidate_period` is clamped to 2..16.

`restir_fold_cut(previous, current)` cuts when `k_eff` differs, and when either frame has a nonzero K and the mode or
feedback differs; with K 0 on both frames it does not cut. `resample` applies it only when the state has history, so
turning Fold on or off or changing its mode cuts. The Fold cut is rule 4 of "Invalidation and R2 integration", Cut
order (`modules/gi_restir/restir_epochs.cpp:57`). `resample` reports every cut through
`RtgiResampleOutput::history_cut`, and `rtgi.cpp` turns it into `CUT_MODULE` when the backend output is denoised and
no other cut applies. A cut clears `F_HISTORY`, so every valid pixel starts under `REJ_CUT` and the bound holds from
that frame.

### Stats, status and harness — `modules/gi_restir/restir_gi_core.cpp`

Pass-stats layout and the `F_PROBE_STATS` gate are under "Statistics". `RestirGiCore::read_stats` takes the Fold
counts from the TEMPORAL rows (the first `group_count` rows). Slots 22 to 26 are `PS_FOLD_REFUSED`, `PS_FOLD_CARRIED`,
`PS_FOLD_RENEWED`, `PS_FOLD_STARTED` and `PS_FOLD_REFUSED_EMPTY`. Slots 27 to 30 are the dump counters (see "Phase-0
counts and start classes"); slot 31 is unused.

`PS_FOLD_REFUSED` counts each rule-1 refusal by event, including one whose reason the fill had already set.
`PS_FOLD_REFUSED_EMPTY` counts, inside the same branch, refusals where `c[0].on` is false; it is a subset of
`PS_FOLD_REFUSED`. `temporal_reject[13]` (`REJ_BOUND`) counts only refusals that kept `REJ_NONE`. Counting rules and
identities for these slots (temporal_reject from INFO_VALID words, renewed equal to refused, carried + renewed +
started equal to the valid traced receivers) are under "Run counters", Row values
(`modules/gi_restir/restir_gi_core.cpp:1111-1115`).

`RestirGiBackend::get_status` (`modules/gi_restir/restir_gi.cpp`) reports `fold_k` (the rounded K), `fold_k_eff`,
`fold_clock`, `fold_pre_frame` and `fold_feedback` of the last frame `resample` ran. In
`modules/gi_restir/restir_harness.cpp`, `params_to_dict` writes `clock`, `k_eff` and `pre_frame`,
`params_from_dict` reads them, `run_case` loads the case's history layers into reservoir set 0 before it runs the
frame, and its stats dict carries `fold_refused`, `fold_carried`, `fold_renewed`, `fold_started` and
`fold_refused_empty`.

### Fold dump variant — `modules/gi_restir/restir_gi_core.cpp`, `modules/gi_restir/shaders/restir_gi.glsl`

The dump and lineage variants of TEMPORAL (binding 32 and bindings 33 to 35, dispatch order, fallback, status fields,
the counters in slots 27 to 30, the history scan, the start histogram and the lineage word) are under "Fold dump and
self-test" and "Fold lineage" (`modules/gi_restir/restir_gi_core.cpp:938-966`,
`modules/gi_restir/shaders/restir_gi.glsl:434-535`). Rule 1 and the write are the same code in all three variants.

### Composition — `modules/gi_restir/shaders/restir_gi.glsl`

- B2k (`F_KEEP_M`): a kept failure (`REJ_VIS` or `REJ_DIRTY`) is present, so it carries and rule 1 tests it; a refusal
  drops the kept M with the rest of `c[1]`. A dirty history is a carry under B2k where B1 and B2 make it a start (see
  "Dirty test").
- Untraced tiles (`F_TILE_SKIP`): the TEMPORAL main stores `rs_empty()` and runs no Fold code, so the next
  traced frame reads M 0 and starts. An invalid receiver takes the same path.
- Sky re-read: the sky twin is MODE_TEMPORAL with `SKY_REREAD`, so it runs the same rule; `rs_store`'s sky
  branch zeroes only Lo.
- VALIDATE runs on the history set before TEMPORAL and keeps the flags on every sample it keeps. Event caps
  lower M before the fill and do not change the class.
- Budget revalidation and ray thinning: a refusal on a pixel with no fresh candidate stores an M 0 reservoir (see "The
  write"); rule 1 still keeps every stored s at or below K - 1, and K - 2 on the pre-frame.

### With Fold off — `modules/gi_restir/shaders/restir_gi.glsl`

At `gi/fold_k` 0, `k_eff` is 0, so `_upload` writes 0 to the clock and K words, bit 19 is clear, rule 1 and the
write do not run, and `restir_fold_cut` does not cut while the previous frame also had K 0. Bits 8 to 15 are
whatever `rs_combine` copied, and nothing else writes them. The TEMPORAL float work is unaffected: `hp` keeps its
values and `h_s`, `h_mv` and `cleared` are integer or boolean locals. Fold's rule and write code sits inside the
`MODE_TEMPORAL` block of `restir_gi.glsl`, so it is compiled only into that mode and its `SKY_REREAD` twin; the
hash and draw helpers in `restir_fold_inc.glsl` are also included by `restir_fold_test.glsl`.

### Footprint — `modules/gi_restir/restir_gi_core.cpp`

The bound uses flags bit 19, reservoir flags bits 8 to 14 (bit 15 free), reject code 13, the `shade_pad1`/`shade_pad2`
words, pass-stats slots 22 to 26, cut reason 8, and the seeds C1 and C2. `RestirFrameUBO`'s size is under "Storage
and frame buffers" (`modules/gi_restir/restir_gi_core.cpp:114`). The bound adds no binding, texture or buffer: the
plain TEMPORAL set 0 is the frame UBO at 0, bindings 1 to 10, 23 (pass stats), 24 (dirty UBO), 26 (events UBO) and 27
(tiles), plus 29 in the `SKY_REREAD` twin, as `run_frame` binds them. The optional Fold dump variant adds
storage-buffer binding 32 (`RESTIR_FOLD_DUMP_BINDING`, `modules/gi_restir/restir_fold_mirror.h`), and the lineage
variant adds image bindings 33 to 35 (see the dump variant section for when each exists).

### Tests — `modules/gi_restir/tests/test_restir_fold.h`

The `[RestirFold]` doctests cover K rounding and clamping, the `k_eff` gate (0 with spatial feedback on, outside the
B modes or at K 0), the pre-frame predicate against `restir_reval_frame` and the `period - 2` form (the 0xFFFFFFFF
wrap included), the upload words (bit 19 always masked from the caller and set only with gated K and a pre-frame;
clock and K 0 at K 0; 7 reads 4; U1 and feedback force 0), the harness dict round trip (clock, k_eff and pre_frame
survive; a dict without them reads 0, 0 and false), the cut rule (a `k_eff` change, or a mode or feedback change
while K is set), the planner order (a Fold change is its own cut, after the request and backend cuts and before the
origin and scene cuts), dirty-box localisation (the planner cuts only inside the dirty boxes when the feed localises
a scene change), and a world change (cuts everything, after the origin cut and before the scene check).

The `Mirror:` tests compare a C++ copy of the shader arithmetic with known answers: the isqrt from a seed one off
either way, the draw at every allowed K (`KB9`: each s gets exactly its weight), rule 1 over every `h_s`, the stored
bound and renewal cap, the bit layout (the write replaces bits 8 to 15 and keeps bits 0 to 7), carry, renewal and
start, the static relock within 3K frames at K 8 and 32, and the hash values (`pcg(0)` 0x07BB2FE2, `pcg(1)`
0xA8BEEA3C, `phi(3, 5)` 0x2D397518, psi_r 0x5072A4E0, psi_s 0xB99D54B9, `draw(psi_s, 8)` 7 and `draw(psi_s, 32)` 13;
`test_restir_fold.h:630-641`). The hash, draw and isqrt copies are in
`modules/gi_restir/restir_fold_mirror.cpp`; the rule 1 and write copies are in the test header. They do not run the
shader. `RestirGiCore::run_fold_test` (`restir_gi_core.cpp`) dispatches
`modules/gi_restir/shaders/restir_fold_test.glsl` over the same draw, phi, psi and isqrt routines and returns its
words, which `modules/gi_restir/restir_probe.cpp` compares with the mirror. That dispatch covers those routines only,
not `rs_temporal_pixel`.

### Limits — `modules/gi_restir/shaders/restir_fold_inc.glsl`

- phi is a hash of the pixel taken mod K; nothing balances the start phases.
- A Fold change during a freeze takes effect and cuts on the first frame after it (see "Test options",
  `modules/gi_restir/restir_gi.cpp:425-455`).
- Ray thinning: a refused thinned pixel keeps coverage 0 until a candidate arrives (see "The write"); the pre-frame
  does not remove the refusal.
- The hash spacing test (`test_restir_fold_rows.h`) covers two seeds only (the default and `F_DETERMINISTIC`), so another seed must not assume it;
  see Run counters, `get_fold_seeds` (`modules/gi_restir/restir_probe.cpp:392`).
- Debug code 60 (`rf.debug_view`, `restir_gi.glsl:1209`) divides the temporal reject code by 11 before `rs_heat`
  clamps it, so `REJ_BOUND` (13) shows saturated.

## TAA jitter in the receivers (`modules/gi_restir/restir_jitter.cpp`)

`modules/gi_restir/restir_jitter.cpp` holds `restir_jitter_shift`, a pure function that turns a pair of
projections into a view-space shift. The receiver decode in the shaders applies that shift to the position
that R2 stores in `geo_position`, so the module decodes receivers at the true (unjittered-surface) point.

- Where the jitter enters (`servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`,
  `servers/rendering/renderer_rd/environment/rtgi.cpp`). R2 passes `scene_data->cam_projection` as `projection`
  and `scene_data->get_cam_projection()` (depth correction plus the TAA jitter offset) as `drawn_projection`.
  `_fill_frame_ubo` builds the frame UBO's inverse projection from `(correction * p_inputs.projection).inverse()`
  with `correction.set_depth_correction(true)`, so the stored `geo_position` is rebuilt without the jitter. R2's
  own RTGI trace does not read that texture for its origin: it uploads `inputs.drawn_projection.inverse()` as the
  trace UBO's inverse projection and `shaders/environment/ddgi_trace.glsl` (MODE_RTGI) rebuilds the position
  from the depth with it.
- Host inputs (`servers/rendering/renderer_rd/environment/gi_module_hooks.h`, `rtgi.h`, `rtgi.cpp`).
  `RtgiFrameInputs` carries `projection`, `previous_projection`, `drawn_projection` and
  `previous_drawn_projection`. `RtgiBuffers` keeps `previous_projection` and `previous_drawn_projection`,
  stored at the end of every frame `RtgiPass` processes. `rtgi.cpp` reads each stored projection when
  `have_previous` is set and the current input otherwise.
- The shift (`restir_jitter.cpp`, `restir_jitter.h`). `restir_jitter_shift(P, D)` forms `M = correction * P`
  (with `set_depth_correction(true)`) and takes `D` as given. Matrix indices are Godot's `columns[c][r]`.
  - If every entry of `D` equals the matching entry of `M` exactly, it returns zero with `unmatched` and
    `jittered` false.
  - It takes the column `c` of `M` whose row-3 entry has the largest absolute value and recovers
    `jitter[r] = (D[c][r] - M[c][r]) / M[c][3]` for r 0 and 1 (`jitter_x`, `jitter_y`, NDC units).
  - It rejects the pair when that row-3 entry is zero, when a jitter entry is not finite, when rows 0 and 1 of
    `D` are not `M`'s rows plus `jitter[r] * M[c][3]` for every column, when rows 2 and 3 differ, when `M`'s
    rows 2 and 3 have an x or y entry (columns 0 and 1), or when the upper-left 2x2 determinant of `M` is too
    small. The comparisons are relative to the magnitude of the entries involved, with
    `RESTIR_JITTER_TOLERANCE` (1e-6).
  - A rejected pair returns all four numbers zero and `unmatched` true. It never returns a partial shift.
  - Otherwise, with `A` the upper-left 2x2 of `M` and `q = A^-1 jitter`, it sets `ax = -qx * M[2][3]`,
    `ay = -qy * M[2][3]`, `bx = -qx * M[3][3]`, `by = -qy * M[3][3]`, and `jittered` true. The mapping is
    `true.x = view.x + view.z * ax + bx`, `true.y = view.y + view.z * ay + by`, `true.z = view.z`. A pair
    with zero recovered jitter returns zeros and leaves `jittered` false. For an orthogonal projection
    `M[2][3]` is zero, so `ax` and `ay` are zero and only `bx` and `by` move the point.
- Params and UBO (`restir_gi.cpp`, `restir_harness.cpp`, `restir_gi_core.cpp`, `shaders/restir_gi_inc.glsl`).
  `RestirFrameParams` has `jitter_shift[4]` and `previous_jitter_shift[4]`, each `(ax, ay, bx, by)` as floats.
  `resample` fills them from the current and previous projection pairs. `params_to_dict` and `params_from_dict` carry
  both as `PackedFloat32Array`; a dict without either key reads zeros. `jitter_shift` and `previous_jitter_shift` are
  the two `vec4` before the eight tuning floats at the end of `RestirFrameUBO` (size under "Storage and frame buffers"), matching `jitter_shift` and
  `prev_jitter_shift` in `RestirFrame`. `_upload` writes both on every pass. `get_status` reports `jitter_shift` (four
  numbers), `jitter_ndc` (the recovered jitter), `jitter_unmatched` (this pair or the previous pair) and
  `jitter_unmatched_frames_total`.
- Shader (`shaders/restir_gi_inc.glsl`). `rs_true_view(view, shift) =
  vec3(view.xy + view.z * shift.xy + shift.zw, view.z)`. `Receiver` keeps `view` (the stored position) and has
  `true_view` besides it. `rs_receiver_cur` (a macro over `rs_receiver`; TEMPORAL, SPATIAL, FINAL, SOURCE) shifts
  with `rf.jitter_shift`; `rs_receiver_prev` (a macro over `rs_receiver`; TEMPORAL, VALIDATE) shifts with
  `rf.prev_jitter_shift`. `rs_receiver` takes the position and normal textures, the rotation, the anchor and the
  shift. Each sets `true_view`, builds `pos` from it, and builds `origin` with `rs_origin_offset_vector` from the true
  centre; each of the four neighbour taps is shifted with its own z, and its w is kept.
- Readers (`shaders/restir_gi.glsl`, `restir_gi_inc.glsl`). Everything that reads `pos` or `origin` reads the
  true point: the plane test in `rs_similar`; `rs_shift_jacobian` and `rs_jacobian_raw`; `rs_phat`; `rs_dir_to`;
  every `rs_visible` ray in TEMPORAL, SPATIAL, FINAL and VALIDATE; the fresh-Lo and replay rays in FINAL; the
  replay distance test in VALIDATE; SOURCE's own-sampler ray; `rs_event_caps` and `rs_hold_caps`; the dirty-box
  segments in `rs_dirty_hit`; and `rs_compat_score` in PASS_SPATIAL_COMPAT, which measures receiver distances
  with `true_view`.
- Unshifted (`shaders/restir_gi.glsl`). TEMPORAL reprojects with `rf.view_to_prev_clip * vec4(r.view, 1.0)`;
  that matrix is built from `correction * previous_projection` and maps a pixel centre to a pixel centre.
  `r.depth` uses the stored z (the shift never changes z), `rtgi_anchor_pixel` works on the pixel grid, PACK
  reads only `geo_pos_cur.w`, and CAPTURE copies the raw textures (a replay applies the shifts from its
  params); CLEAR, SKY_EPOCH and the debug views use no position. The candidate decode keeps R2's hit point and
  normal as stored, since R2 traced them from the true origin; it takes only `depth` and `nrm` from the
  receiver.
- Invariants (`shaders/restir_gi_inc.glsl`, `restir_gi.cpp`). The shift has no setting and is always applied.
  With both shifts zero every added operation is `x + z * 0 + 0`, which compares equal to `x` for finite `x`.
  R2 writes a zero position with w 0 for a pixel without geometry; the shift maps any stored position to a
  finite point, and the TEMPORAL, SPATIAL, FINAL, SOURCE and VALIDATE passes test `valid` (`g.w > 0.5`) on the
  receivers they build.
- Freeze: PACK sees the last real frame's shifts; see "Test options" (`modules/gi_restir/restir_gi.cpp:425-449`).
- Doctests (`modules/gi_restir/tests/test_restir_jitter.h`, "[RestirJitter] ..."). A perspective camera
  (60 degrees, 16:9, 0.05 to 2000), an off-axis frustum, and two orthogonal cameras, each with the jitter
  `(0.3 / 640, -0.45 / 360) * 2` and its three sign flips, and with D built as `get_cam_projection` builds it
  (`set_depth_correction` plus `add_jitter_offset`, then `* P`). The recovered jitter matches within
  `JITTER_TOLERANCE` (1e-12 for an 8-byte `real_t`, 1e-6 otherwise). For five uv pairs and five depths the shift
  maps the `inverse(M)` rebuild onto the `inverse(D)` rebuild within `MAP_TOLERANCE` of the point's length
  (1e-9 for an 8-byte `real_t`, 1e-4 otherwise), and the zero shift does not. Zero jitter gives exact zeros
  with neither flag set, and `D == M` gives zeros. A y-flipped D is unmatched and zero, with and without
  jitter; a depth change, a zoom and another camera are unmatched and zero. A positive x jitter moves the
  point toward -x in view space and a positive y jitter toward +y; the orthogonal shift has `ax == ay == 0`
  with non-zero `bx` and `by`. The harness round trip keeps both shifts, and a dict without them reads zeros.
  The map tests call a C++ copy of `rs_true_view`, so they test the function and the formula, not the shader.
- Known limits (`restir_jitter.cpp`, `shaders/restir_gi_inc.glsl`, `restir_gi.cpp`).
  - The origin offset in `rs_origin_offset_vector` (`origin_scale` ULPs plus the raster footprint) has no term
    for the shift. `origin_floor` is 0 by default and is read as `MAX(setting, 0)` (`restir_gi.cpp`).
  - The anchor is `rtgi_anchor_pixel` (`shaders/environment/rtgi_inc.glsl`: trace pixel * scale + scale / 2,
    clamped to the screen); the shift corrects the jitter only, not the depth sample's own footprint inside a
    trace pixel.
  - `rs_origin_offset_vector` rotates a tap normal with the current camera's rotation for previous receivers
    too, so with a turning camera a previous receiver's offset points slightly off its normal.
  - An unmatched pair gets a zero shift, so that frame keeps the stored-position error.
  - `restir_jitter_shift` builds `correction` with `set_depth_correction(true)`, a second copy of R2's
    correction in `_fill_frame_ubo`. A drawn projection built with a different correction, such as without the
    y flip, is unmatched.
  - R2's own passes still read the stored position: in `shaders/environment/rtgi.glsl`,
    `rtgi_previous_trace_pixel`, the temporal plane test and the DDGI lookup.

## Run counters

The per-frame record is defined in `modules/gi_restir/restir_test.h`; `modules/gi_restir/restir_probe.cpp` exposes it
and `modules/gi_restir/restir_fold_rows.cpp` packs the Fold rows. They are CPU-side readbacks, and no project setting
reads them. The `readback` test option sets `RESTIR_F_PROBE_STATS` in the flags word and the `fold_dump` test option
swaps the TEMPORAL dispatch (`restir_gi.cpp` `resample`; see "Fold dump and self-test").

- Frame records (`restir_test.h`). `RestirFrameRecord` is `frame` (uint32), `stats` (`RestirFrameStats`) and
  `fold` (`RestirFoldRecord`, 32 int64 fields, -1 for not available): tag, serial, clock, k, k_eff, mode,
  feedback, cut, cut_reason, pre_frame, reval, history_used, trace_w, trace_h, hp_eq_px, expected_phase0,
  s_max, n_over, hist_texels, lineage_refusals, lineage_unknown, lineage_families, phase0_start, refused_off,
  start_cut, start_frame, start_plane, start_normal, start_class, start_no_sample, start_vis, start_other.
- What fills them (`restir_gi.cpp` `resample`). A record is built only when the `readback` test option is on. It fills
  frame and stats, then the first fourteen Fold fields: tag = `options.fold_tag`, serial = `state.serial`, clock =
  `params.fold_clock` (the state's `frames` before the increment), k/k_eff/mode/feedback from this frame's
  `RestirFoldFrame`, cut and cut_reason from the epoch plan (reason 0 when there is no cut), pre_frame =
  `params.fold_pre_frame`, reval = `RESTIR_F_REVAL_FRAME` in `params.flags` (set as under Fold, "Clock, pre-frame and
  cuts", `modules/gi_restir/restir_gi.cpp:583-586`; `RESTIR_F_REVAL_SHARE` is a different bit), history_used =
  `use_history`, and trace_w and trace_h from the trace size. When `fold_dump_ran` and `read_fold_dump` succeed it
  also fills hp_eq_px, expected_phase0, phase0_start and refused_off from `stats`, s_max, n_over and hist_texels from
  the dump, and the eight start_* fields from `restir_fold_start_sums`, then adds the dump's starts to the start
  totals. When `fold_lineage_ran` and `read_fold_refused_ids` succeed it fills the three lineage fields from
  `restir_fold_families_add`.
- Fold dump variant: see "Fold dump and self-test" (`modules/gi_restir/restir_gi_core.cpp:938-966`); `fold_dump_ran`
  and `fold_lineage_ran` gate the dump and lineage record fields above.
- get_records (`restir_probe.cpp`). It copies the list under `restir_frame_records_mutex()` and builds its array
  outside the lock. It returns a dictionary with `stride`, `count` and `data`; the stride is 35: frame, then
  the stats fields from `geometry` to `compat_h_sum`.
- Pixel sums (`restir_gi.cpp` `_accumulate_pixels`, `restir_probe.cpp` `get_pixel_sums`, `restir_test.h`). With
  readback on and a record index inside [`accumulate_from`, `accumulate_to`), `resample` calls `_accumulate_pixels`,
  which reads radiance, coverage and info and, for every pixel whose info word has `INFO_VALID`, adds luma (0.2126,
  0.7152, 0.0722) to `output_luma`, 1 to `valid`, and 1 to `covered` when coverage is above 0; `sums.frames` rises by
  1 per call. With `pixel_detail` it also fills seven per-pixel arrays (`cand_luma`, adding the half in bits 16 to 31,
  and `cand_n` when bit 15 is set; `out_spatial` and `n_spatial` when bit 9 is set, else `out_temporal` and
  `n_temporal` when bit 8 is set; `blocked` from bit 10) and `reject_temporal` (info bits 0 to 3) and `reject_spatial`
  (bits 4 to 7), 16 counters per 8x8 block (block index = (y / 8) * blocks_x + x / 8). `get_pixel_sums` returns
  `width`, `height`, `frames`, `output_luma`, `covered`, `valid`, the seven detail arrays and the two reject arrays;
  `reset_pixel_sums` clears them, and they also reset when the trace size changes.
- Fold rows (`restir_probe.cpp` `get_fold_rows`, `restir_fold_rows.cpp`). `get_fold_rows` returns a
  `PackedInt64Array` of `RESTIR_FOLD_ROW_FIELDS` (42) values per record, in this order: record, frame, tag,
  serial, clock, k, k_eff, mode, feedback, cut, cut_reason, pre_frame, reval, history_used, trace_w, trace_h,
  n_valid, cut_rej, had_history, refused, renewed_raw, started, carried, refused_empty, hp_eq_px,
  expected_phase0, s_max, n_over, hist_texels, lineage_refusals, lineage_unknown, lineage_families,
  phase0_start, refused_off, start_cut, start_frame, start_plane, start_normal, start_class, start_no_sample,
  start_vis, start_other. `get_fold_row_fields` returns those names in order (`restir_fold_row_names`).
  `restir_fold_pack_row` builds one row. `record` is the index in the current record list: it restarts at 0
  after `reset_records`, and every `get_fold_rows` call packs the whole list.
- Row values (`restir_fold_rows.cpp`, `restir_gi_core.cpp` `read_stats`, `shaders/restir_gi.glsl`).
  - n_valid = `restir_fold_n_valid`, the sum of all 16 `stats.temporal_reject` entries. `read_stats` fills that
    histogram only for info words with INFO_VALID (bit 14). TEMPORAL writes info 0 on an untraced tile and on
    an invalid receiver. SPATIAL replaces bits 4 to 7 and may set bit 9; FINAL sets bits 10 to 12 and, with the
    pixel-detail option, bit 15 and the bits above it. So bit 14 and the low four bits are TEMPORAL's. The
    population is the frame's valid traced receivers with geometry: sky pixels and untraced tiles are in
    neither n_valid nor the four counts, and a ray-thinned valid pixel is counted. `stats.geometry`
    (get_records column 1) counts the same set from FINAL, so for one record n_valid and geometry are equal, and a row
    where they differ is inconsistent.
  - cut_rej = `restir_fold_cut_rej`, `temporal_reject[9]` (REJ_CUT). had_history = `stats.had_history`
    (INFO_HAD_HISTORY, bit 13). TEMPORAL starts the reason at REJ_CUT and replaces it only when
    `RESTIR_F_HISTORY` (set from `use_history`) is on (`rs_temporal_pixel`, `shaders/restir_gi.glsl`), so cut_rej
    equals n_valid when history_used is 0, and is 0 when history_used is 1.
  - `use_history = !cut && options.history` (`restir_gi.cpp`): a cut always gives history_used 0, and with the
    `history` test option off history_used is 0 while cut can still be 0.
  - refused, renewed_raw, started, carried and refused_empty are pass-stats slots 22, 24, 25, 23 and 26, read
    from the TEMPORAL rows as doubles cast to int64. The engine exports the raw slots.
  - renewed_raw is added to in the renew branch of `rs_fold_bits`, which runs for every pixel the rule-1
    refusal cleared and for no other pixel; the refusal counter is added to at the same place, so renewed_raw
    equals refused on every frame, and a derived "renewed" class renewed_raw - refused is 0.
  - Identity: on a frame with K_eff above 0, refused + (renewed_raw - refused) + started + carried == n_valid,
    because every valid traced receiver calls `rs_fold_bits` once and adds 1 to exactly one of carried,
    renewed_raw and started. On K_eff 0 frames all four counts are 0; with K_eff above 0 and no history,
    started == n_valid.
- Mode and cut reason (`restir_gi_core.h` `RestirMode`, `restir_epochs.h` `RestirCutReason`). mode: 0 B1, 1 B2, 2 U1,
  3 B2k. cut_reason is the `RestirCutReason` value (0 none, 1 first, 2 request, 3 origin, 4 backend, 5 scene without
  feed, 6 scene overflow, 7 scene unlocalised, 8 Fold, 9 world; `modules/gi_restir/restir_epochs.h:40-49`); the test
  order is under "Invalidation and R2 integration", Cut order (`modules/gi_restir/restir_epochs.cpp:37-92`). R2's own
  cuts (a resize, a camera move over 25 m in `rtgi.cpp`, a backend that did not run the frame before) reach the module
  as `history_cut` and read 2, as does a forced history reset.
- Mutex (`restir_test.h`, defined in `restir_gi.cpp`). `restir_frame_records_mutex()` guards the push in
  `resample`, the copy in `get_fold_rows` and `get_records` (both pack outside the lock), and the clear in
  `reset_records`.
- Tag (`restir_probe.cpp` `configure`, `restir_test.h`). `configure({"fold_tag": t})` sets the test option;
  every record and the status key fold_tag carry the tag of the frame that ran. The option is read on the
  render thread without a lock, so a change lands on the next frame that runs.
- State serial and totals (`restir_gi.cpp` `begin_frame`). `RestirGiState::serial` is set from
  `state_serial_next` (the first state gets 1) when `begin_frame` makes a state and its allocation succeeds. A
  view seen for the first time adds 1 to `state_creates_total`; a view whose state exists but is unallocated or
  has another trace size adds 1 to `state_rebuilds_total`. A failed allocation moves neither the total nor the
  serial; the retry that succeeds counts once, as a rebuild, because the entry stays in the map. A released
  view has no state, so its next frame is a create. Every state starts at frames 0 with a RESTIR_CUT_FIRST
  frame, so the serial changes exactly where the clock restarts.
- release_once (`restir_gi.cpp` `begin_frame` and `release`). `configure({"release_once": true})` sets a one-shot
  option. The next `begin_frame` that passes the refusals before the state allocation (no buffers,
  missing inputs, shader variant, own sampler, the event_reshade checks) clears it, calls
  `release(p_inputs.buffers)`, adds 1 to `release_tests_total`, then makes the state again in the same call with
  another serial. It sits after those refusals but before the allocation, so a frame refused for `allocation failed`
  has already spent it and a frame refused earlier has not. It sits after the no-buffers test because
  `release(nullptr)` frees every state and the core. `release_calls_total` counts `release(p_buffers)` calls with a
  non-null pointer that found a state; `release(nullptr)` is not counted. `release_once` is a plain bool written from
  `configure` and read on the render thread; a write between the read and the clear is lost.
- Status keys (`restir_gi.cpp` `get_status`), among others: fold_feedback, fold_tag, state_serial (0 before any
  frame), state_creates_total, state_rebuilds_total, release_calls_total, release_tests_total.
- R2 release counters (`servers/rendering/renderer_rd/environment/rtgi.cpp`, `rtgi.h`). `RtgiPass` has two
  static `SafeNumeric<uint64_t>` counters, only incremented and read by `_write_status`:
  `release_free_data_total`, incremented in `RtgiBuffers::free_data` after `backend->release(owner)`, and
  `release_pass_total`, incremented in `RtgiPass::release` after `backend->release(buffers)`. Both count only
  calls that reached a registered backend; `RtgiPass::release` also returns early when the buffers hold no RTGI
  custom data. `free_data` counts also when the module had no state for those buffers, so it is weaker proof
  than the module's `release_calls_total`. `_write_status` writes the top-level keys rtgi_release_free_data and
  rtgi_release_pass to the status file at rtgi/debug_status_path. The module never reads them.
  - Which path moves which (`servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp`).
    The GI block calls `RtgiPass::process` only when `render_gi`, a valid normal-roughness slice, DDGI custom
    data and `RtgiPass::is_enabled()` (rtgi/enabled) all hold, and calls `RtgiPass::release` otherwise.
    `release` ends by clearing the RTGI context and custom data. `free_data` is called on the custom data only
    from `RenderSceneBuffersRD::cleanup`, which runs from the destructor, `configure` and
    `configure_for_reflections`; `RtgiBuffers` has no destructor that calls it, and `cleanup` does not erase
    the custom data entries. So the pass path moves rtgi_release_pass by 1 and rtgi_release_free_data by 0,
    and a SubViewport resize moves rtgi_release_free_data by 1.
  - Reading them. Both totals are process-wide and cover every viewport, while the status file is written per
    buffers, so read the difference across an event rather than the absolute value. Both stay 0 with no backend
    registered. One `RtgiPass::release` call cannot fire both paths, so both keys moving by 1 across one event
    means two separate releases.
- Seeds and CPU mirrors (`modules/gi_restir/restir_fold_mirror.h` and `.cpp`, pure). `restir_fold_pcg`,
  `restir_fold_rng_state`, `restir_fold_phi`, `restir_fold_isqrt_from`, `restir_fold_isqrt` and
  `restir_fold_draw` are engine copies of the shader's arithmetic. `restir_fold_draw` returns 0 when
  `(K * K) >> 2` is 0, that is for K below 2. Constants: `RESTIR_FOLD_C1` (0x68E31DA4), `RESTIR_FOLD_C2`
  (0x2C1B3C6D), `RESTIR_FOLD_DIM_RENEW` (0), `RESTIR_FOLD_DIM_START` (1) and `RESTIR_FOLD_DETERMINISTIC_SEED`
  (0x1234567). `restir_fold_rand_seed(seed)` = `restir_fold_pcg(seed * 0x9E3779B1 + 0x7F4A7C15)`, the seed
  `rs_rand` and `rs_rand_frame` hash (`shaders/restir_gi_inc.glsl`).
- `restir_fold_other_seeds(rtgi_seed, seed_offset)` (`restir_fold_mirror.cpp`) lists every seed sum another
  `rtgi_rng_state` caller can use, in order: C1; rtgi_seed + 0..7 (`rtgi_rand2` in R2's MODE_RTGI, which doubles
  the dimension); only when seed_offset is not 0, rtgi_seed + seed_offset + 0..7 (the module's SOURCE, which
  runs on params.seed = rtgi/seed + seed_offset); `restir_fold_rand_seed(rtgi_seed + seed_offset)` + 64..575;
  and `restir_fold_rand_seed(0x1234567)` + 64..575. That is 1033 values at offset 0 and 1041 otherwise. Both
  hashed blocks are always listed, since the deterministic setting can switch between them.
- Probe `get_fold_seeds` (`restir_probe.cpp`) returns c1_seed, c2_seed, other_seeds (a `PackedInt64Array` from
  `restir_fold_other_seeds` with the live rtgi/seed and the seed_offset test option), rtgi_seed, seed_offset,
  deterministic and rand_seed_live (the hashed seed `rs_rand` uses under those settings). It reads rtgi/seed
  and seed_offset at the call, while R2 reads rtgi/seed through `GLOBAL_GET_CACHED`.
- Doctests.
  - `modules/gi_restir/tests/test_restir_fold.h` ("[RestirFold] ..."): see Fold, "Tests".
  - `modules/gi_restir/tests/test_restir_fold_rows.h` ("[RestirFoldRows] ..."): the constants;
    `restir_fold_rand_seed(1)` 0xF5F17964 and `(0x1234567)` 0x56F88133; other_seeds at (1, 0) gives 1033 values
    with C1 first, 1..8 next and the two hashed blocks at their ends, and every value at ring distance 5 or
    more from C2 and C2 + 1; at (1, 3) it gives 1041 values with the module's 4..11 and the offset seed hashed;
    draw at K 0 and 1 gives 0; n_valid and cut_rej over a planted histogram (1..16
    gives 136 and 10); the 42 names in order; a planted record packs to the known row; a default record packs
    -1 in every Fold field and 0 in every count; and the split and start-class fields pack after
    lineage_families with the earlier fields in place.
- Known limits (`restir_gi.cpp`, `restir_test.h`).
  - fold_tag is read without a lock; with several views `release_once` releases the first view that begins a
    frame, and fold_tag is one value for the process.
  - Records and the totals are per process; `reset_records` clears records, never totals.
  - The rows carry the module's cut reason only; R2's cut kind for the frame is not in them.
  - R2 reads rtgi/resolution_scale through `GLOBAL_GET_CACHED` (`rtgi.cpp`), so a change may take a later frame
    to change the trace size; state_rebuilds_total moves when it does.

## Fold dump and self-test

The dump and self-test live in `modules/gi_restir/shaders/restir_gi.glsl`,
`modules/gi_restir/shaders/restir_fold_inc.glsl`, `modules/gi_restir/shaders/restir_fold_test.glsl`,
`modules/gi_restir/restir_gi_core.cpp`, `modules/gi_restir/restir_gi.cpp` and
`modules/gi_restir/restir_probe.cpp`. Dump GLSL is compiled only into shader objects that define
RS_FOLD_DUMP; without a dump buffer the frame runs the plain TEMPORAL pass, which binds and computes nothing
extra. When `RestirGiResources::fold_dump` exists, the fold-dump variant replaces the TEMPORAL dispatch:
`RestirGiCore::run_frame` (`restir_gi_core.cpp:938-966`) builds the dump set 0, clears the buffer, calls
`_dispatch_dump` and calls the plain `_dispatch(PASS_TEMPORAL or PASS_TEMPORAL_SKY)` only when `_dispatch_dump`
returned false (`if (!dumped)`, line 962). It never runs both. When the lineage images also exist the lineage
variant is tried first (line 952) and the dump variant is the fallback (line 959).

- Shared include (`modules/gi_restir/shaders/restir_fold_inc.glsl`): constants, phi, draw, isqrt and the PSI macros
  are described under Fold, "Draw and hashes". It declares no bindings; `restir_gi.glsl` includes it inside the
  TEMPORAL block after RS_FOLD_CLOCK and RS_FOLD_K, which alias the Fold UBO words (see Fold, "Clock, pre-frame and
  cuts"), and `restir_fold_test.glsl` includes the same file, so the self-test runs the same functions as TEMPORAL.
- Binding 32, declared only for MODE_TEMPORAL with RS_FOLD_DUMP (`restir_gi_inc.glsl`): std430
  `RestirFoldDump { uint s_max; uint n_over; uint texels; uint pad; uint starts[]; }`.
- Slots 27 to 30 of the pass-stats rows (row 0 is TEMPORAL) are defined under "Phase-0 counts and start classes"
  (`modules/gi_restir/shaders/restir_gi_inc.glsl:105-108`).
- History scan. `rs_fold_dump_scan(px)` runs at the top of TEMPORAL `main`, before the traced-tile test. At
  K 0 it returns, so s_max, n_over and texels stay 0. Otherwise every in-grid thread reads
  `hs = (rs_load(px).flags >> 8) & 0x3F` from the history reservoir (binding 7) and accumulates into shared
  memory; invocation 0 then does one atomicMax (s_max) and two atomicAdds (n_over, texels) on the buffer.
  n_over counts hs > K - 1; texels counts in-grid texels.
- Relock counts. In `rs_temporal_pixel`, `rs_fold_dump_relock(px, hp, had)` is called right after `had` is
  formed (`had = c[1].present || cleared`). For an hp == px receiver when K is not 0, slot 27 adds 1. Slot 28
  adds 1 when the receiver is at phase 0 ((RS_FOLD_CLOCK + phi(px)) mod K == 0) and `had`; slot 29 adds 1 at
  phase 0 without `had`. hp stays -1 without F_HISTORY.
- Start histogram. Inside `if (RS_FOLD_K != 0)`, after `rs_fold_bits` writes the flags, when `!had`:
  s = (flags >> 8) & 0x3F after the K - 2 pre-frame cap; u = (RS_FOLD_CLOCK + phi(px)) mod K; c is the class
  of the TEMPORAL reason (table under "Phase-0 counts and start classes");
  `atomicAdd(starts[(c * 64 + s) * 64 + u], 1)`. On a pre-frame every class index is offset by 8, so read a
  start's class together with the row's pre_frame field.
- Dump objects (`restir_gi_core.cpp`, `RestirGiCore::_ensure_dump`, `_dispatch_dump`). Each variant gets a
  `RestirGiShaderRD` in `RestirFoldDumpObjects` (heap, built on the first dispatch inside `_dispatch_dump`).
  Defines are the core's
  general defines plus `#define RS_FOLD_DUMP`; modes are MODE_TEMPORAL and, for SW and HW, MODE_TEMPORAL +
  SKY_REREAD (VARIANT_NONE has only the first). The dump has its own pipelines; PassMode, PASS_MAX, the
  pipeline table, marker names and `_dispatch` do not include it. Set 0 is the TEMPORAL set plus binding 32,
  set 1 the ray scene (not for VARIANT_NONE), set 2 (sky mode) the shade set, all through UniformSetCacheRD
  with the dump shader's RID. Timestamps "ReSTIR Temporal Fold Dump" and "ReSTIR Temporal Sky Fold Dump". If
  the objects cannot be built or the shader, a pipeline or a set is invalid, `_dispatch_dump` returns false,
  the plain TEMPORAL pass runs and `fold_dump_ran` is false. A build that fails is tried once per variant and
  core (`dump_tried`), so `fold_dump_active` stays false afterwards.
- Buffer. `RestirGiResources::fold_dump` is RESTIR_FOLD_DUMP_BYTES = 262,160 bytes: a 16-byte header plus
  16 * 64 * 64 words. `ensure_fold_dump(true)` creates it and logs "fold_dump" in alloc_log, which
  `get_status` reads; `ensure_fold_dump(false)` frees it and removes the entry; `free_all` frees the buffer
  only, and the entry goes when `allocate` clears alloc_log (`restir_gi_core.cpp:185`). `run_frame`
  clears it with `buffer_clear` just before the dump dispatch and sets `fold_dump_ran` only when the dump
  pipeline was bound and dispatched. `read_fold_dump` reads the header and starts with `buffer_get_data`.
- Backend (`restir_gi.cpp`). `resample` calls `ensure_fold_dump(options.fold_dump && options.readback)`. In the
  readback block, when `fold_dump_ran` and `read_fold_dump` succeeds, the record takes hp_eq_px, expected_phase0,
  phase0_start and refused_off from those slots, s_max, n_over and hist_texels from the header, and adds starts to
  `restir_fold_starts` (a uint64 per start slot, see Buffer, under `restir_fold_starts_mutex`, declared in
  `restir_test.h`). Status: fold_dump_active (this frame's dump pass ran; also true on a lineage frame, because the
  lineage pass is the dump pass, `restir_gi_core.cpp:965`) and fold_dump_frames_total. The dump exists only while
  readback is on; starts accumulate across every view and dump frame in the process.
- Self-test (`restir_fold_test.glsl`, `RestirGiCore::run_fold_test`). Its own `RestirFoldTestShaderRD` with
  one variant, built on the first run. Push constant FoldTestParams { sub, k, count, v0, grid_w, grid_h,
  stride, frames } (8 uint, 32 bytes); set 0 binding 0 FoldTestOut { uint words[]; }. Local size 64 x 1 x 1;
  groups = ceil(threads / 64), x = min(groups, 1024), y = ceil(groups / x);
  t = group.y * x * 64 + global.x.
  - sub 0 draw: t < count, words[t] = draw(t, k); count words.
  - sub 1 phi: t < grid_w * grid_h, atomicAdd(words[phi(px) mod k], 1); k words.
  - sub 2 psi: t < count * frames, i = t / frames, v = v0 + t mod frames, linear = i * stride, pixel
    (linear mod grid_w, linear / grid_w); words[t] = (phi mod k) | (psi_r mod k) << 8 | draw(psi_s, k) << 16;
    count * frames words.
  - sub 3 isqrt: t < count; a miss adds 1 to words[0] and does atomicMin(words[1], t); 2 words starting
    {0, 0xFFFFFFFF}.
  - `restir_fold_test_invalid` (`restir_fold_mirror.cpp`) rejects sub outside 0..3; k not a power of two in
    4..64; count 0 except for sub 1; grid 0 for subs 1 and 2; frames or stride 0, or the last pixel
    ((count - 1) * stride) outside the grid, for sub 2; more than 2^24 threads; more than 2^22 words.
    `restir_fold_test_mirror` runs the same subs on the CPU; `restir_fold_test_compare` counts differing words
    plus a size difference and reports the first differing index (the common length when only sizes differ).
- Probe methods (`restir_probe.cpp`). `configure({"fold_dump": true})` enables the dump and readback; false
  disables the dump and leaves readback as it was. `run_fold_test(args)` posts the test to the render thread,
  using the probe's own `RestirGiCore`, not the backend's; on success it also runs the CPU mirror for the
  comparison. `fold_test_ready()` and `get_fold_test_result()` read it; `mirror_fold_test(args)` runs the CPU
  mirror alone on the calling thread, with no rendering device needed. Arg keys and defaults: sub 0, k 8,
  count 0, v0 0, grid_w 0, grid_h 0, stride 1, frames 1. Result keys: source ("gpu" or "mirror"), ok, error,
  sub, k, count, v0, grid_w, grid_h, stride, frames, word_count, words (PackedInt64Array), mirror_mismatches,
  first_mismatch. A GPU run sets mirror_mismatches to compare(GPU, mirror) and first_mismatch to its first
  index (-1 when none); a mirror run sets mirror_mismatches to 0; a failed run leaves both at -1. Values
  outside 0..2^32 - 1 give ok false with error "argument out of range"; without a rendering device the GPU run
  gives "no rendering device".
- Fold starts. `get_fold_starts()` returns the accumulated counts (size and index: see Buffer; zeros before any dump
  frame); `get_fold_starts_frames()` counts dump frames added since the last reset; `reset_fold_starts()` clears both,
  and `reset_records` does not. `mirror_fold_starts(info, width, k, clock, pre_frame)` (`restir_fold_start_mirror`)
  builds the counts a cut frame gives: for every texel with the valid bit (1 << 14), s = draw(psi_s(px, clock), k)
  capped at K - 2 on a pre-frame, u = (clock + phi(px)) mod K, class 0 (8 on a pre-frame). It returns empty when the
  bytes are not a multiple of 4, there are no texels, width is not positive or does not divide the texel count, k is
  not a power of two in 4..64, or clock is negative. `restir_fold_start_mirror` itself returns an all-zero array for a
  null input, a width not above 0, or such a k.
- `run_case` (`restir_harness.cpp`) reads the fold_dump test option. When it is on, the case sets
  F_PROBE_STATS, creates the dump buffer in its own resources, and returns result["fold_dump"] = {active,
  k_eff, clock, n_valid, cut_rej, had_history, hp_eq_px, expected_phase0, phase0_start, refused_off, s_max,
  n_over, texels, starts}; without a dump frame s_max, n_over and texels are -1 and starts is empty.
- Data contracts. On a dump frame with K not 0, hist_texels = trace_w * trace_h. A row with s_max, n_over or
  hp_eq_px at -1, or with K not 0 and hist_texels not trace_w * trace_h, has no dump data and must not be
  measured. Over a window, the sum of `get_fold_starts()` over all classes equals the sum of the rows' started
  over the same dump frames, and `get_fold_starts_frames()` equals the number of dump frames. On a cut row the
  class 0 total (class 8 on a pre-frame) equals n_valid and started. Starts are added before the row is
  pushed. At K 0 the scan and relock return early, texels is 0 and slots 27 to 30 are zero.
- Doctests: `modules/gi_restir/tests/test_restir_fold_dump.h` ("[RestirFoldDump] ...") pins the class table, the
  buffer size, binding and slot constants, start sums (a short array refused), compare (planted one-word, two-word and
  short-array differences), draw weights at K 4 to 64, the isqrt sub over 2^24, phi counts summing to the grid, a psi
  word packing its three fields, argument checks, the accumulator (adds two frames, ignores a short array) and the
  cut-frame mirror (skips texels without the valid bit, no s past K - 1 or the pre-frame cap, all counts zero for a K
  that is not a power of two). `test_restir_fold.h` is described under Fold, "Tests"; `test_restir_fold_rows.h` under
  "Run counters", Doctests.

## Fold lineage

The lineage pass and its images live in `modules/gi_restir/shaders/restir_gi.glsl`,
`modules/gi_restir/shaders/restir_gi_inc.glsl`, `modules/gi_restir/restir_gi_core.cpp`,
`modules/gi_restir/restir_fold_mirror.cpp`, `modules/gi_restir/restir_gi.cpp`,
`modules/gi_restir/restir_probe.cpp` and `modules/gi_restir/restir_test.h`. Every lineage GLSL statement is
under `#ifdef RS_FOLD_LINEAGE`; the constants in `restir_gi_inc.glsl` sit inside the existing
`#ifdef RS_FOLD_DUMP` block, and the image declarations require MODE_TEMPORAL, RS_FOLD_DUMP and
RS_FOLD_LINEAGE together. No other variant or dump object defines RS_FOLD_LINEAGE.

- Shader objects. Lineage uses a third set of TEMPORAL objects per variant (`RestirGiCore::lineage_objects`, the same
  `RestirFoldDumpObjects` type, heap, created on the first lineage dispatch). Defines are the core's general defines
  plus `#define RS_FOLD_DUMP` and `#define RS_FOLD_LINEAGE`; modes are MODE_TEMPORAL and, for SW and HW, MODE_TEMPORAL
  + SKY_REREAD. The lineage pass also runs everything the dump pass does (see "Fold dump and self-test").
  `_ensure_dump` and `_dispatch_dump` take a lineage flag; PassMode, PASS_MAX, the pipeline table and marker names do
  not include it. Timestamps "ReSTIR Temporal Fold Lineage" and "ReSTIR Temporal Sky Fold Lineage". An object that
  fails to build is tried once per variant and core (`lineage_tried`); the dump object then runs in its place and
  fold_lineage_active stays false (reason code 3, see "Probe and status").
- Bindings (set 0, lineage objects only): 33 `rs_fold_lineage_in` (rg32ui readonly), 34
  `rs_fold_lineage_out` (rg32ui writeonly), 35 `rs_fold_refused_id` (r32ui writeonly). The lineage images
  ping-pong with the reservoir index: TEMPORAL reads reservoir[hist] and lineage[hist] and writes
  reservoir[hist ^ 1] and lineage[hist ^ 1]. `run_frame` takes the lineage path only when hist is below 2 and
  `fold_refused_id` is valid, so two images suffice.
- Lineage word (rg32ui, `rs_fold_lineage_store`). x = birth pixel index + 1 = y * trace_w + x + 1; y = the
  birth clock (RS_FOLD_CLOCK). Per in-grid thread in TEMPORAL:
  - not traced, no valid receiver, or K 0: word (0, 0), id 0;
  - start (!had): word (own pixel + 1, v), id 0;
  - carried (had and not cleared): word = lineage_in at hp, id 0;
  - refusal (cleared): id = lineage id of lineage_in at hp, and the renewal writes (own pixel + 1, v).
  - Every in-grid texel of both output images is written on every lineage frame, so no stale word survives.
- Lineage id (r32ui, `rs_fold_lineage_id`). id = ((v_birth & 0xFF) << 24) | (pixel + 1), or 0xFF000000 when the word's
  pixel field is 0 or above 0xFFFFFF. It is never 0 and it is an exact encoding, not a hash. A start draws s below K,
  K is at most 64 and a carry adds 1 to s until the refusal at s + 1 >= K (K - 1 on a pre-frame), so a refused lineage
  is at most K frames old and 8 clock bits keep two lineages of one pixel apart. Lineage runs only when trace_w *
  trace_h <= 0xFFFFFF (RESTIR_FOLD_LINEAGE_MAX_TEXELS, 16,777,215 texels); above that no images are made (reason code
  2, see "Probe and status"). CPU helpers: `restir_fold_lineage_pixel` and `restir_fold_lineage_id`
  (`restir_fold_mirror.cpp`).
- Resources. `RestirGiResources::fold_lineage[2]` (R32G32_UINT, trace size) and `fold_refused_id` (R32_UINT)
  are made by `ensure_fold_lineage(true)` and freed by `ensure_fold_lineage(false)`, `free_fold_lineage` or
  `free_all`. alloc_log entries: "fold_lineage 0", "fold_lineage 1", "fold_refused_id" (added by
  `ensure_fold_lineage`, removed by `free_fold_lineage`, so `free_all` removes them too). The backend calls
  `ensure_fold_lineage(fold_lineage && fold_dump && readback)` right after `ensure_fold_dump`.
  `run_case` never creates them, so it runs the plain dump.
  - The images count as present only when all three RIDs are valid. If one fails, all three are freed,
    `fold_lineage_failed` is set, lineage stays off (code 3) and one warning is printed (WARN_PRINT_ONCE). While the
    flag is set nothing is created again. The flag clears in `free_all` and when lineage is no longer wanted.
  - Every `ensure_fold_lineage` call sets `fold_lineage_refusal`; `run_frame` sets 3 after a failed lineage dispatch
    (`modules/gi_restir/restir_gi_core.cpp:248-264, 955`); the backend copies it to
    `RestirGiSummary::fold_lineage_refusal`.
- Stale words. `fold_lineage_written` holds the index of the image the last lineage dispatch wrote
  (hist ^ 1), or -1. Before a lineage dispatch, if it is not hist, `run_frame` clears lineage[hist] to 0 with
  `texture_clear`, so a carried word reads as unknown rather than as an older frame's word. It is -1 after
  creation, after `clear_history`, after any frame without the lineage pass and after `free_fold_lineage`; it
  differs from the next hist whenever history_index did not become the written image (spatial feedback).
  - The first frame after a cut has F_HISTORY off, so every valid receiver starts and writes its own word.
  - `allocate` calls `free_all`, which frees the images; `ensure_fold_lineage` makes them again.
  - `restir_fold_cut` (see Fold, "Clock, pre-frame and cuts") reports a cut when k_eff changes; while k_eff is 0 the
    lineage pass writes word 0 everywhere.
  - Lineage enabled mid-run: until the next cut, carried pixels keep word 0 and their refusals count as
    unknown.
- Read-back and families (`restir_gi.cpp`). When the lineage pass ran and readback is on, the readback block
  reads `fold_refused_id` with `texture_get_data` (trace_w * trace_h * 4 bytes, on every lineage frame) and
  calls `restir_fold_families_add(options.fold_tag, ids, texels)`. `read_fold_refused_ids` returns the
  read-back bytes and the texel count without a second copy. The call groups the ids outside any lock, then,
  under `restir_frame_records_mutex`, adds them to that tag's `RestirFoldFamilyTag { sizes: HashMap size ->
  count, totals: {frames, refusals, unknown, families} }` in `restir_test.h`. It returns the frame's
  {refusals, unknown, families}, which become row fields 29, 30 and 31 (lineage_refusals, lineage_unknown,
  lineage_families). They are -1 on a row without the lineage pass or with a failed read-back. `run_case`'s
  fold_dump dictionary does not carry them. Families are added before the row is pushed.
  - `restir_fold_families(ids, count, r_sizes)` (`restir_fold_mirror.cpp`, pure) skips 0; every other id is a
    refusal; an id whose low 24 bits are 0 is unknown and is not grouped; equal remaining ids form a family,
    and `r_sizes[size]` is incremented once per family. Returns {refusals, unknown, families}. Families are
    per frame: a family is the set of receivers that clear one lineage id on one frame.
  - Row fields 29 to 31 hold the lineage counts; fields 32 to 41 hold phase-0 and start values, and
    RESTIR_FOLD_ROW_FIELDS is 42.
- Probe and status (`restir_probe.cpp`, `restir_gi.cpp`).
  - `configure({"fold_lineage": true})` enables lineage only when fold_dump is on, including when the same
    call set fold_dump first (fold_dump is read first). Otherwise lineage stays off with options refusal code
    1. Any later fold_lineage key that is not refused sets the code to 0. `configure({"fold_dump": false})`
    also turns lineage off.
  - Reason codes (RestirFoldLineageRefusal): 0 none, 1 lineage asked without the dump, 2 trace grid above
    16,777,215 texels, 3 lineage images or shader objects unavailable. The options store an int, not a
    string. `get_status` reports the options code when it is not 0, otherwise the render side's last code,
    and builds the text with `restir_fold_lineage_refusal_text`.
  - `get_fold_families(tag)` returns PackedInt64Array (family_size, count) pairs with sizes ascending.
    `get_fold_family_totals(tag)` returns {frames, refusals, unknown, families}. A tag with no lineage frame
    gives empty pairs and zeros. `reset_fold_families()` clears every tag; `reset_records` and
    `reset_fold_starts` do not touch families, and `reset_fold_families` does not touch records or starts.
  - Status fields: fold_lineage_active (this frame's lineage pass ran), fold_lineage_frames_total,
    fold_lineage_refusal (text, empty for code 0) and fold_lineage_refusal_code (int).
- Data contracts.
  - Give each measured group its own fold_tag. Totals are per tag, nothing resets at a cut, and a reused tag
    merges frames. Families accumulate per tag over every view and lineage frame in the process, cut frames
    included (they add a frame but no refusals).
  - For a tag, `get_fold_family_totals(tag).frames` equals the number of its rows whose lineage_refusals is at
    least 0; refusals equals the sum of those rows' refused and of their lineage_refusals; unknown equals the
    sum of their lineage_unknown; families equals the sum of their lineage_families. The tag added is the
    row's own tag (record.fold.tag, read once from options.fold_tag).
  - A row's lineage fields are valid only when fold_lineage_active was true for that frame; a row without
    the pass carries -1. Lineage is not part of `run_case` or the case files.
- Doctests: `modules/gi_restir/tests/test_restir_fold_lineage.h` ("[RestirFoldLineage] ...") pins bindings 33
  to 35; the pixel word and id packing, including unknown for pixel 0, above the mask and above the texel
  limit; the known answer {5, 5, 5, 7, 0, 7, 9} giving 6 refusals, 3 families and sizes {3: 1, 2: 1, 1: 1}
  (twice: {3: 2, 2: 2, 1: 2}); unknown ids counted and not grouped; ids over a 64 x 36 grid and 66
  consecutive clocks all distinct; the accumulator (adds frames, counts an empty frame, sums totals),
  per-tag pairs and totals, an unknown tag giving zeros, and reset clearing both.

## Phase-0 counts and start classes

These parts are in `modules/gi_restir/shaders/restir_gi.glsl`, `modules/gi_restir/shaders/restir_gi_inc.glsl`,
`modules/gi_restir/restir_fold_mirror.cpp`, `modules/gi_restir/restir_fold_mirror.h`,
`modules/gi_restir/restir_test.h`, `modules/gi_restir/restir_fold_rows.cpp`,
`modules/gi_restir/restir_gi_core.cpp`, `modules/gi_restir/restir_gi.cpp` and
`modules/gi_restir/restir_harness.cpp`. Every shader statement is under `#ifdef RS_FOLD_DUMP` or runs only
when the dump ran, so plain TEMPORAL variants are unaffected.

Phase 0 is the frame on which a pixel's staggered restart clock reaches 0, that is `(RS_FOLD_CLOCK + phi(px)) mod K`
is 0; the slots below count receivers at that phase (`PS_FOLD_PHASE0`,
`modules/gi_restir/shaders/restir_gi_inc.glsl:105-108`).

- Slots (pass-stats row 0, stride 32):
  - 27 PS_FOLD_HP_EQ_PX: every hp == px receiver when K is not 0.
  - 28 PS_FOLD_PHASE0: a phase-0 receiver ((RS_FOLD_CLOCK + phi(px)) mod K == 0) with hp == px and `had` set
    (c[1].present or cleared).
  - 29 PS_FOLD_PHASE0_START: the same phase-0 hp == px receivers without `had`.
  - 30 PS_FOLD_REFUSED_OFF: in the refusal branch that adds to slot 22 PS_FOLD_REFUSED, receivers whose own
    pixel is not at phase 0 ((RS_FOLD_CLOCK + phi(px)) mod K != 0). There is no hp == px condition.
  - `rs_fold_dump_relock` takes had as its third argument and selects slot 28 or 29, so slot 28 plus slot 29
    is the full phase-0 hp == px count.
  - C++: RESTIR_FOLD_SLOT_HP_EQ_PX 27 .. RESTIR_FOLD_SLOT_REFUSED_OFF 30 (`restir_fold_mirror.h`); `read_stats` adds
    them to RestirFrameStats::fold_hp_eq_px, fold_phase0, fold_phase0_start and fold_refused_off; they are 0 without a
    dump. A static_assert checks slot 30 against the stride.
- Start classes: CUT 0, FRAME 1, PLANE 2, NORMAL 3, CLASS 4, NO_SAMPLE 5, VIS 6, OTHER 7, plus PRE 8 on a
  pre-frame; 16 classes (RESTIR_FOLD_START_CLASSES). Reason to class: 9 CUT, 1 FRAME, 2 PLANE, 3 NORMAL,
  4 CLASS, 10 NO_SAMPLE, 11 VIS, and every other code (0, 5 to 8, 12 to 15) OTHER, so no start is lost.
  `rs_fold_start_class` (shader) and `restir_fold_start_class` (`restir_fold_mirror.cpp`) hold the same
  table; the reason-code constants are RESTIR_FOLD_REJ_*_CODE. The shader constants RS_FOLD_START_CUT.._OTHER
  are 0..7, _PRE 8, _S 64, _U 64.
- Start buffer: size and index `(c * 64 + s) * 64 + u` are under "Fold dump and self-test", Buffer
  (`modules/gi_restir/restir_fold_mirror.h:60-65`); the shader's starts[] is a runtime array sized from
  `RESTIR_FOLD_START_COUNT` or `RESTIR_FOLD_DUMP_BYTES`, as are buffer creation, `buffer_clear`, `read_fold_dump`, the
  module accumulator, `get_fold_starts`, `mirror_fold_starts` and `run_case`'s starts.
- `restir_fold_start_sums(starts, sums)` (`restir_fold_mirror.cpp`, pure) gives eight per-class totals over s
  and u, folding class c + 8 into c. It returns false and sets every total to -1 when the array is not
  65,536 words.
- Rows (`restir_fold_rows.cpp`): RESTIR_FOLD_ROW_FIELDS 42. expected_phase0 is field 25 and holds slot 28.
  Fields: 32 phase0_start, 33 refused_off, 34 start_cut, 35 start_frame, 36 start_plane, 37 start_normal,
  38 start_class, 39 start_no_sample, 40 start_vis, 41 start_other. They are -1 unless the frame's dump was
  read, like s_max. The start sums are that frame's dump starts with pre-frame twins folded in, not the
  accumulator.
- `run_case`'s fold_dump dictionary carries phase0_start and refused_off, from the stats like hp_eq_px.
- Identities: hp_eq_px >= expected_phase0 + phase0_start; the eight start sums add up to the row's started on
  a dump row, because every !had start is in exactly one class; refused_off <= refused.
- Doctests: `test_restir_fold_dump.h` also pins the class table for every reason 0 to 15, plain and pre-frame, the
  class ids, (15, 63, 63) = 65,535, and the cut mirror in class 0 (class 8 on a pre-frame); `test_restir_fold_rows.h`
  is described under "Run counters", Doctests.
