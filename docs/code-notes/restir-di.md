# Resampled direct lights

`modules/gi_restir_di` (built by default, `module_gi_restir_di_enabled=no` drops it; resampling is off until
`resampled/enabled`) picks each pixel's head lamps and tail samples before DI-1's trace. DI-1 then runs in
external-entries mode and the opaque pass shades heads, tail samples or nothing.

- `environment/direct_resampler_hooks.h:DirectResampler::set_singleton` is the one registration point. Core never includes the
  module. `render_forward_clustered.cpp` calls `DirectShadows::prepare_lamps`, then the resampler's `run`, then
  `DirectShadows::process` with the entry texture; with no resampler, or the setting off, `process` runs as before.
  `free_device_resources` is called from the renderer's destructor, because the module's servers-level uninit runs after
  the rendering device is gone.
- The lab, its settings (`debug/lab_log`, `debug/no_stamp`, `ka8_*`), the lab log and mask, the `LAB_COUNTS` variants
  and the lab test exist only with `scons gi_restir_di_lab=yes`, which writes `di_resampler_config.gen.h` with
  `DI_RESAMPLER_LAB`. The four test switches and the `[DirectSampler]` reference tests stay in every build. The lab
  blocks in `di_resample.glsl` and `di_sampler_inc.glsl` stay as text, so the forward shader's source is the same.
- Profile: `run` records timestamps and labels "Direct Resample Entries" and, with spatial reuse, "Direct Resample
  Spatial", each around its own compute list. After a successful `run`, `_pre_opaque_render` records "Direct Shadows
  Trace" and opens a label of that name, because labels are flat: an inner end clears the label rather than restoring
  "Direct Shadows". "Direct Shadows Trace" covers the trace alone; the clamp and the two filter passes keep their own
  spans. With resampling on, the span that "Direct Shadows" covers without it is the sum of the spans that exist: Direct
  Shadows, Direct Resample Entries, Direct Resample Spatial (only with spatial reuse) and Direct Shadows Trace. With the
  resampler off nothing changes: one "Direct Shadows" span and label.
- `direct_lights.h:GpuSlot` is 48 bytes. It gains the anchor offset (reset position minus live position, world, so the
  entry pass ranks a flickering lamp where it was last reset) and the mean colour: linear colour times render energy at
  the lamp's mean energy, with fade and exposure, as `LightData.color` is built. A colour rather than a mean-to-live
  ratio, because live energy is 0 at a flicker trough. `RenderingServer.light_set_direct_mean_energy` sets the mean
  energy; -1 (the default) means the live energy.
- `Params.head_entries` is DI-1's own entry count. Outside external mode it is entries - 1, so DI-1 is unchanged. In
  external mode the slot layers are heads, then tails, then the rest layer at entries - 1. Tail layers hold slot 0, so a
  pipeline without bit 15 never matches one and falls back to the rest ratio.
- The second filter pass has two variants. `FILTER_SECOND_EXTERNAL` (`EXTERNAL_ENTRIES`) has the head and tail split
  and the tail block; `FILTER_SECOND` reads entries - 1 as both the head count and the rest layer and has no tail block,
  as at the base. `_filter` takes the external one only in external mode. Outside it the two give the same values,
  because head_entries is entries - 1 and tail_visibility is 0, so a change to one must keep that.
- The forward shader tests bit 15 (`sc_use_direct_resampled()`) at every use of the route: the record read, the route
  call in both lamp loops, the layer test in `direct_shadow_factor`, the tail color in `light_process_omni` and
  `light_process_spot`, and the `debug/log` mark. With bit 15 at 0 each is a constant branch once the pipeline is
  specialized, as DI-1's bit 14 test at its call is, so the off path does not depend on the driver proving the route's
  globals constant through the lamp loop.
- Entry texture (r32ui array, written by the module, read by DI-1 at set 0 binding 20): layers 0 to H-1 head slots, then
  three layers per tail sample: slot with the half-float blue rest scale in the top 16 bits, the share-out weight as
  float bits, then red and green rest scales as two halves. The rest scale is the sample's colored target over the
  tail's colored target sum, times its resampling weight Sum p / p_y (times the candidate count m in exact mode); DI-1
  adds visibility times it to the rest ratio. Every tail sample of a pixel gets the same share-out weight (Sum p / K, or
  Sum p / m in exact mode) and DI-1 floors them with one dynamic flag, so its picks are uniform across the samples,
  which that scale assumes. Shares of p_k in exact mode would have let DI-1's floors over-weight a dim lamp in the
  ratio.
- Record (rg32ui array, binding 54, K + 1 layers): layer 0 holds the packed octahedral normal and the raw depth as float
  bits (depth 0 means no record); layer 1 + k holds the sample's slot and its shading weight. The opaque pass divides
  that weight by K. A negative weight means the live energy arm: the tail sample is shaded with the live colour instead
  of the mean. A tail of K lamps or fewer is shaded exactly, each lamp weight K.
- DI-1's external mode: `consider` reports every lamp it walks to `add_external`, with zero-weight lamps too, so a tail
  sample at a flicker trough still finds its position and gets a ray; only lamps with weight count toward the rest. A
  pixel is empty only when no own entry, no rest lamp and no tail sample were seen. The share-out skips its ray loop
  when there is nothing to pick.
- `di_sampler_inc.glsl` holds what the entry pass and the opaque loop must compute the same way: the lamp window and
  cone, `di_in_set` (term above 0 and the decoded record normal facing the lamp), the octahedral codec, the target, the
  inversion and the race. The entry pass tests and ranks with the normal after it has been packed and unpacked, as the
  opaque pass reads it from the record, so both see the same lamp set.
- `scene_forward_clustered.glsl:direct_route` decides the set with two comparisons on the record point and normal, with
  no `di_geometry`: a lamp is in the set when d2 times inv_radius squared is below 1 and the record normal's dot with
  the lamp offset is above 0, where d2 is the squared distance from the record point. An owned lamp in that set that
  matches a pick routes to it; one that matches none is skipped. This drops the window, the cone and the length that
  `di_in_set` uses, so frames are no longer bit-identical to the entry pass's set: a lamp at the very edge of its
  range, a spot lamp whose cone misses the record point, and a lamp near the horizon of the record normal can land on
  the other side. Each of those carries almost no light at that point. An overflowed d2 counts as out (plain shading),
  a NaN as in.

  The picks load lazily, on the first lamp that is in the set, so a pixel with none pays no fetch:
  - Head slots go through `scene_forward_lights_inc.glsl:direct_own_load`. That is the cache `direct_shadow_factor`
    already fills, moved unchanged into a helper. R16_UNORM gives an integer c over 65535, and `round(x * 65535.0)`
    returns c, so the uint compare matches the float compare it replaces.
  - Tail slots go into `direct_tail_slots`: every k below K, empty entries included. A real slot is at least 1, so an
    empty entry never matches. `direct_tail_pass` still fetches the weight at layer k + 1 on a match.

  The head loop is bounded by `direct_record_heads`, never by `direct_own`, which counts heads and tails. Loops run to
  the constant 7 with a count guard, so the arrays stay in registers once unrolled. `direct_record_ok` also requires
  heads and K to be at most 7. process() rejects more than 8 entries, so that guard never changes a frame.
  The route block also excludes `MODE_UNSHADED`: `direct_own_load` lives in `scene_forward_lights_inc.glsl`, which an
  unshaded pass does not include. The lamp loops, and so every call of the route, are already outside unshaded passes.
- `di_sampler_inc.glsl:di_candidate` drops a target below `DI_MIN_TARGET`, 2^-64 (5.421011e-20 rounds to it exactly),
  like a zero target: it is never added to a total and never picked, by the inversion, the race or the exact tail.
  The exact tail still records such a lamp, with no share and so no ray of its own; the opaque pass lights it at
  factor 1 under the pooled visibility of the rest. Without the drop, the race key E / w misbehaves at tiny w:
  - at w of about 1e-38 to 5e-38 the quotient overflows, and `di_precise_divide`'s correction turns it into NaN, a key
    that never wins;
  - at denormal w below about 2.9e-39, 1 / w is infinite, so the correction gives +inf or -inf, and a -inf key always
    wins.

  At w >= 2^-64 and E <= 16.64 (the exponential's largest value), every key, 1 / w and `di_weight` stay finite while
  the total is below 1.8e19. The CPU references use the same constant. The bounds were checked in float32 outside the
  engine.
- With reservoirs off, `di_resample.glsl` walks the cluster cell three times: head ranking by target at the mean colour
  and the anchor (ties to the lower slot), the tail's sums and its first K lamps, then K inversions on the tail when it
  has more than K lamps. The prior is last frame's factor for a lamp DI-1 held in an entry there, else the rest ratio
  for its mean colour, else 1, found with DI-1's reprojection and guide test. Tail u values come from a hash of pixel,
  frame and sample, apart from DI-1's ray stream.
- Known-answer switches, all off by default, under `resampled/debug/` (known-answer cases KA1 to KA6 and KA2-q):
  - `tail_visibility` sets what DI-1's tail rays return: Traced (the ray), Visible (1, no ray), or Hashed (1 with
    probability rho = 0.2 + 0.7 * ((slot * 40503) mod 65536) / 65536, by a hash of pixel, frame and slot, so a lab can
    scale each lamp's energy by the same rho for KA2-q's exact image). The random numbers are drawn either way, so the
    rays after a tail ray see the same stream in every mode. With `direct/force_factor_one` on, Hashed tail layers keep
    their hashed value in both the unfiltered write and the second filter pass; every other layer reads 1. Visible and
    Hashed tail rays aren't traced, so they don't count in `counters.rays`. A tail lamp that gets no ray in a frame
    takes the rest mean (`direct_shadows.glsl`, the `tail_seen` branch), which in Hashed mode is the mean of the other
    lamps' hashed values, not its own rho. So Hashed is a known answer only when every tail lamp gets a ray every frame,
    that is at E_h 0 and K_t 1 (KA2-q's setting).
  - `stale_prior` makes the prior 0 for even slots (KA2's forced stale prior), so those lamps carry only the floor.
  - `log` is a file path. While it is set, the entry pass counts into a 256 KB buffer (4 header words, then one word per
    slot): pixels with at least one tail lamp, and per slot every head entry, every exact tail lamp with a weight above
    0 and every sampled pick. The next frame reads it back (a stall, debug only) and writes one line: `frame F size WxH
    tail_pixels N`, then `slot:count` for each slot counted. Only the first viewport to render in a frame counts; the
    others run without the count and without the check below. A path change and a device release write the pending line
    first, and the file stays open across a device release, so a re-made device appends.
  - The same setting turns on the in-no-layer check. At a pixel whose tail was shaded exactly, the entry pass sets the
    sign bit of the record's depth word (the depth there is always above 0; the opaque loop masks it). The opaque loop
    then marks any owned, in-set lamp that is neither a head nor a tail lamp by writing (16384, 0, 16384) over the
    pixel's colour, so the count reads as marked pixels, which is 0 exactly when the lamp count is. Lamps cut by the
    instance's layer mask or the lightmap skip are tested after those, so they are never marked. A head lamp DI-1 sees
    at weight 0 (the known edge below) is marked too, and so can a lamp exactly on the set's range or cone edge, because
    the entry pass rebuilds the point from its own float inverse of the projection: a lab reads the marked pixels, not
    just their count. Labs run it with glow and auto exposure off, since a marked pixel at 16384 would bloom and move
    the exposure.
- Design calls beside the ones above: E_h reaches the opaque pass from texture sizes (DI-1's slot layers - 1 - the
  record's tail layers), with no new uniform; DI-1's external mode still walks the cell for the rest key, target and
  flags with no streaming pick, a second cell walk that is part of the measured cost; tail layers skip DI-1's history
  match, clamp and both filter passes, as center and as neighbor, and tail rays don't write the ray record, as rest
  rays don't.
- The hit mode's race (`di_race_*`, `di_hit_pick`) keys on a lamp id. `_pack_light` writes the light instance's RID
  local index into `RaySceneLight.pad`, the same low 32 bits DI-1's K4 keys lamps by, and 0 is a real id. Core doesn't
  use the hit mode yet. KA8 has an id-0 case and fails any accessor that returns repeated ids.
- The entry pass's shader object is made on its first frame, not at module start: `ShaderRD`'s constructor reads the
  rendering device, which doesn't exist yet at the servers init level. `DirectShadows::get_history` hands over last
  frame's slots, factors and guide (the buffer written before this frame's flip) and refuses history after a slot-table
  generation change.
- Known edges, left: under the live energy arm a lamp whose mean colour is 0 has target 0, so it is never a tail sample
  although it is lit live. This is the default arm. A head lamp that DI-1 sees at weight 0 (live colour 0, or facing
  away with the full normal while the packed one faces it) is in no layer, so the opaque pass skips it; its light there
  is about 0.

## Reservoirs

Used with `resampled/reservoirs` at B1 or U1 and one tail sample. With reservoirs Off the entry pass writes what it
wrote before.

- A pixel with more candidate tail lamps than `c_full` cuts them into `c_full` strata, draws one candidate per stratum
  and picks one of the draws; that fresh pick has mass `c_full`. A pixel with fewer tail lamps takes the path above.
- Temporal reuse: the entry pass folds in the previous frame's pick for the same surface, found by reprojection where
  the previous depth and normal match, with its mass cut to `m_temporal_cap`. A stored mass is cut to `m_stored_cap`.
- Spatial reuse: a second variant of the same shader combines the pick with up to `spatial_neighbors` picks at random
  points within `spatial_radius` pixels (at a render height of 1440, scaled with the height), only where the neighbor
  holds a pick this frame and its depth and normal are similar.
- B1 combines by mass alone, which is biased. U1 combines with pairwise MIS, weighting each candidate by its target at
  the other pixel's surface, and leaves the visibility prior out on reservoir pixels.
- Reservoir records are 12 bytes per pixel in two swapped sets with a 7-bit frame stamp, so a set that was not written
  last frame is never read.
- `head_rank` TV multiplies a lamp's target by its shadow factor from the previous frame (at least `floor`) and keeps
  last frame's head lamps in place: a challenger replaces the weakest head lamp only after it has led by
  `head_swap_margin` for `head_swap_frames` frames in a row.
- Lab build only: `debug/lab_log` writes counters and a settings line, with a per-pixel mask of the reservoir path
  beside it (also written at shutdown); `debug/no_stamp` reads the stored reservoir without the frame-stamp test.

## L0 mirror (every build) and KA8 lab (lab option)

- `modules/gi_restir_di/di_sampler_reference.h` mirrors di_sampler_inc.glsl operation for operation: `hash` is
  `di_hash`, `candidate` is `di_candidate`, `weight` is `di_weight`, `Inversion` and its functions are the
  `di_inversion_*` functions, `race_exponential` is `di_race_exponential`, and `race_k` is the key
  `di_race_exponential` computes first. di_resample.glsl draws the tail inversion's goal with `stream_uniform`, with
  reservoirs or without. A change to any of these GLSL functions changes its mirror in the same commit; the
  `[DirectSampler]` tests run the mirror.
  hash_inverse undoes hash; KA8 uses it to build seed hashes with k = 0 and k = 2^23 - 1. stratum_weight,
  contribution_weight and source_pdf_weight are the design's stratum and source-pdf formulas, templated so the L0 tests
  run them in exact rationals.
- KA8: `resampled/debug/ka8_request` (path to a JSON report) and `resampled/debug/ka8_mutant` (None, Index Key, Loop
  Ties, K24, No Log, Hit Strata; default None). Served once per distinct path from DirectResamplerRD::run or ::release,
  as DI-1's dump_request is. Builds the test shader di_sampler_test.glsl twice (variant 0 screen shape: LightData
  records through a cell list and a slot table; variant 1 hit shape: RaySceneLight records), both through di_hit_pick,
  over the hit records and without rays.

## Cold frames (test switch)

- `rendering/global_illumination/direct/debug_cold_frames` N: in the traced pass, a lamp whose age (frame minus its
  reset frame) is under N loads no history for its own entries and a ray drawn for one is not traced, so the entry
  shades 1. Off with resampled entries and in reference mode. The slow-start test arm uses 6.
