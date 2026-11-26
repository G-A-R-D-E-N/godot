# Ray scene alpha cut-outs

`servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene` can trace alpha-tested (cut-out) surfaces as
cut-outs: rays pass through the holes of foliage, grates and fences instead of stopping on the whole triangle. It is
off by default. The UV buffer, alpha store, `RAY_SCENE_ALPHA` shader define and bindings 9, 10, 13 and 14 are always
built, so the setting can be toggled live; while `rendering/global_illumination/ray_scene/alpha_test` is false, every
material slot carries alpha index 0, so traces, material bytes, hashes, feed states and the hardware split match the
off build.

The DDGI trace shader reads cut-outs in all four of its modes (probe, surfel, debug rays and the per-pixel ray-traced GI
trace `TRACE_MODE_RTGI`), and in the shadow rays those modes trace. The ReSTIR GI module's ray shader
(`modules/gi_restir/shaders/restir_gi.glsl`) gets the alpha define in every mode (the sky epoch pass traces no ray, so
it is unused there), and so do its fold-dump and lineage shaders, so every ray the module traces tests the same
cut-outs: the candidate rays of its own sampler (`MODE_SOURCE`), the visibility tests of temporal and spatial reuse and
their sky re-read forms, and the re-trace and re-shade rays of FINAL and VALIDATE. In Stochastic mode its re-tests draw
the same random value as the ray that made the sample only where they pass its seed (see "Bindings and defines"). The
ray-traced direct shadow pass (`direct_shadows.cpp`) builds its trace shaders without the alpha define, so it does not
apply cut-outs. A cut-out surface casts a solid shadow there, while DDGI and ReSTIR GI rays pass through its holes.

## Settings (`servers/rendering/rendering_server.cpp`)

- `RenderingServer::init` defines five settings in the ray scene group, next to `backend` and
  `skip_additive_surfaces`. `doc/classes/ProjectSettings.xml` documents them.

| Setting (`rendering/global_illumination/...`) | Default | Read |
|---|---|---|
| `ray_scene/alpha_test` | false | every `RayScene::update`, latched once at its start: no restart |
| `ray_scene/max_alpha_layers` | 512 (16 to 2048, clamped by the store) | every `RayScene::update` (`RayAlphaStore::configure`) |
| `ray_scene/alpha_mode` | 0 Stochastic, 1 Threshold | `RayScene::alpha_shader_defines`, when the trace shaders are created: restart |
| `ray_scene/alpha_layer_size` | 256 (256 or 512) | every `RayScene::update` |
| `ray_scene/alpha_fill_texels_per_frame` | 67108864 | every `RayScene::update` |

- A change of layer count or layer size makes `RayAlphaStore::configure` release the store and start empty. Every
  surface traces solid until its layer is filled again. A change of the texel budget alone keeps the store.

## Store (`servers/rendering/renderer_rd/environment/ray_scene_alpha.cpp`)

- `RayAlphaStore` (with `ray_scene_alpha.h` and `shaders/environment/ray_scene_alpha_fill.glsl`) holds one coverage
  layer per (texture, cutoff) pair and a table of entries that point into it.
- `RayAlphaStore::_ensure_texture`: one R8 layer per pair in a 2D array texture with one mip, `max_layers` layers of
  `layer_size` squared, allocated in full on the first update: 32 MiB at the defaults, 128 MiB at 512 squared. It
  creates the 128 squared R8 8-mip mean array (`mean_texture`, 21,845 B per layer, 11,184,640 B at 512 layers) and
  its 8 per-mip storage views (`mean_mip_views[0..7]`) in the same call, freeing everything on any partial failure.
- `RayAlphaStore::acquire`: the layer key `LayerKey` is the whole 64-bit texture RID and the cutoff, 1 to 255. A
  cutoff of 0 or above 255, a null RID, and a scale or offset that is not finite return index 0 (no layer). It
  returns the table entry index, or 0 when the texture is already freed or no layer can be had. Entries are deduped
  on (layer, canonical `EntryKey`) and the layer's generation (see "Entry key and cull" below). The 4-argument
  `acquire(source, cutoff, scale, offset)` is an inline overload that builds a key with `bits` 0 (cull disabled, far
  mode 0). A hash collision appends a second entry, which is never found
  again and ages out with the rest. Calling it every frame for every material (the material cache is rebuilt every
  frame) adds nothing after the first time and stamps the layer and the entry with the frame.
- `RayAlphaStore::TableEntry` (56 B, binding 13 in the trace shaders): layer, state, UV scale, UV offset, then the
  key's `bits` and far values, `density` and `far_state` (layout under "Entry key and cull" below). Entry 0 is the
  null entry, all zeros.
- `RayAlphaStore::update`: needs a RenderingDevice. It marks a layer PENDING when its source RD texture is missing,
  when its signature (RD texture id and size) differs from the filled one, or when it is not READY. It fills pending
  layers on the GPU within the per-frame texel budget (always at least one, writing both the hard coverage layer and
  mean mip 0), marks each READY through `commit_fill`, then dispatches mips 1 to 7 of the mean array for READY layers
  that want far values and whose `far_filled_signature` differs (also charged to the fill budget, 21,844 reads per
  layer), copies layer states into the table entries through `sync_entries` (entry 1 up, so the null entry never takes
  layer 0's state) and uploads the table when anything changed. Free layer slots are skipped. `commit_fill`,
  `layer_needs_far`, `commit_far` and `sync_entries` need no device; `tests/servers/rendering/test_ray_scene_alpha.cpp`
  drives them in `Alpha store resets the old texture's entry before a new texture reuses it`, `Alpha change serial stays
  still when nothing a ray reads changes`, `Alpha far trigger follows mode 1 or 2 use and refills`, `Alpha far state
  publishes on the next begin_update` and the other store tests.
- `ray_scene_alpha_fill.glsl:main`: in default mode, when the source is at least `layer_size` on both axes, each layer
  texel is the share of mip 0 source texels in its footprint whose `round(alpha * 255) >= cutoff`. Otherwise (either
  axis smaller) it is one sample of the source with the LINEAR, REPEAT sampler at the layer texel centre, set to 1 when
  `alpha * 255 >= cutoff` and to 0 otherwise. Both branches accumulate the raw alpha sum and sample count in shared
  memory and write the 128 squared mean mip 0 without changing the hard cut share. Under `MODE_MEAN_MIP`, each thread
  averages a 2 by 2 block from `source_mip` into `dest_mip` for one layer.
- A layer whose source RD texture goes missing turns PENDING. When the same texture comes back, its signature equals
  the filled one, but the layer is not READY, so it is refilled.
- `RayAlphaStore::fill_shader` is a pointer, made in `_ensure_pipeline` and deleted in `release`. The `ShaderRD`
  constructor asks the RenderingDevice for its vendor name, so a member shader would need a device to build the
  store. With the pointer, `configure` and `acquire` run with no device, as in
  `tests/servers/rendering/test_ray_scene_alpha.cpp`'s `Alpha store keys, entries and clamps need no device` and
  `Alpha entry key equality, canonicalisation and invalid far values`. Each pipeline gets
  a fresh shader object, because `ShaderRD::initialize` refuses a second call on the same object.

## Store: layer and entry lifetime

- Textures send no free signal (`TextureStorage::texture_free` tells only the decal and area-light atlases), so the
  store polls. `RayAlphaStore::set_liveness` takes a function. `RayScene` passes `_alpha_texture_live`, which is
  `TextureStorage::owns_texture`: false for a freed slot and for a slot reused under a new validator. With no function
  set (as in `Alpha store keys, entries and clamps need no device`, `Alpha entry key equality, canonicalisation and
  invalid far values` and `Alpha table entry layout and mode 1 byte offsets` in
  `tests/servers/rendering/test_ray_scene_alpha.cpp`) every texture counts as live.
- `RayAlphaStore::begin_update(frame)` runs in `RayScene::update` right after `configure`, before the instance loop,
  with `RSG::rasterizer->get_frame_number()`. It resets `layers_refused` and `dead_sources_refused`, releases every
  layer whose texture fails the liveness test, and retires every entry whose layer was released or whose last use is
  more than one frame back.
- `RayAlphaStore::_release_layer`: erases the layer's key, clears the slot (EMPTY, no source, sizes and signatures 0),
  increments its `generation` and puts it on `free_layers`. A reused slot is EMPTY, so the refill test queues it and
  the new entry copies EMPTY: nothing samples the old texels before the full-slice fill overwrites them.
- `RayAlphaStore::_retire_entry`: erases the entry's key only when it still maps to that entry, writes the tombstone
  (`TableEntry()` with `layer = ENTRY_RETIRED`: EMPTY and every other field zero) and puts the index on
  `free_entries`. Entries never shrink
  outside `configure` and `release`, because `RayScene::_surface_uv_unsafe` reads an entry before
  `RayScene::_alpha_live` bounds the index, and `LocalVector::operator[]` traps on a bad index. The shader treats the
  tombstone as solid like any entry that is not READY. `entry.layer < layers.size()` keeps it out of the state sync.
- When a new pair needs a layer, `acquire` takes a free slot, else appends below `max_layers`, else evicts the live
  layer with the oldest last use among those not used in this frame or the one before (ties go to the lower index,
  any state), and retires that layer's entries at once. With every layer used in the last two frames it refuses.
  The two-frame rule covers several `RayScene::update` calls in one frame (several viewports): what one viewport used
  carries this frame's stamp, so a later viewport in the same frame cannot evict it.
- Bounds: live layers stay at most `max_layers`. Walking back into a cell whose textures stayed loaded costs no refill
  unless their layers were evicted. Live entries stay at about two frames' working set, so an animated offset does
  not add one entry per frame for good, and the table buffer stops growing once its capacity covers the peak.
- Cost per update: one `owns_texture` call per live layer and one pass over the entries. The victim scan runs only on
  a miss at the cap.
- `Stats`: `layers` (live layers), `layer_slots` (slots, free ones included), `layers_ready`, `layers_pending`,
  `layers_refused`, `table_entries`, `entries_free`, `dead_sources_refused`, `source_missing_frames`, the per-frame
  and total fill and fill texel counts, the totals `layers_released_total`, `layers_evicted_total` and
  `entries_retired_total`, and `far_invalid` (acquires this frame whose far values were not finite, reset in
  `begin_update`). `RayScene::_print_alpha` adds the released, evicted and retired totals and `far_invalid` to its line.
- Limits: the stamp is the engine frame, so any number of updates in one frame protect each other's layers. When the
  layers used this frame and the frame before exceed `max_layers` (several viewports over disjoint regions), the
  later acquires are refused and those cut-outs trace solid until the sets fit. A viewport that updates less often
  than every other frame can lose its layers to eviction between its updates and refill them.

## Change serial

- `RayAlphaStore::get_change_serial` returns a 64-bit counter. `RayScene::get_alpha_change_serial` returns the same
  value. It stays 0 while alpha is off, because the store is never configured.
- It moves when, in a frame, something a ray reads through the alpha test may have changed:
  - an entry is added in `acquire` (a new pair, a new scale or offset, a reused entry index);
  - an entry is retired (`_retire_entry`: aged out, its layer released, or its layer evicted);
  - a layer is released (`_release_layer`: its texture was freed, or it was evicted at the cap);
  - a layer's state changes (`update` marks it PENDING, `commit_fill` marks it READY);
  - a layer is filled with a new signature, READY to READY included (`commit_fill`);
  - an entry's state is copied from its layer (`sync_entries`);
  - a configured store is dropped (`release`, also through a `configure` with a new layer count or size).
- After a state change, the first `begin_update` of a later frame moves it once more. `RayScene::update` builds this
  frame's materials between `begin_update` and the store's `update`, and `RayScene::_alpha_instance` keeps an entry
  index in a material only when that entry is READY at that time. A layer filled in frame N is therefore traced as a
  cut-out from frame N + 1, and the serial moves in both frames. `state_change_frame` keeps the frame of the last
  state change, so another `begin_update` in frame N (a second viewport) does not take the move meant for N + 1.
- It does not move for: an `acquire` that finds an existing, unchanged entry; the `last_used` stamps; a `commit_fill`
  with the signature the READY layer already holds; an `update` with no layer to change; a `configure` with the same
  clamped layer count and size; `release` on a store that is not configured; stats.
- Some moves change nothing a ray sees (EMPTY to PENDING, and retiring an entry no material uses). They are allowed,
  since a reader only needs the serial to move whenever traced cut-outs change.
- In a steady scene it can still move: with two viewports that update at different rates, the entries of the one that
  is idle for more than a frame age out and are acquired again on its next update. Each of those moves is a real
  table change.
- It covers the store's state only. Whether a material carries an entry index at all can change without the store:
  a mesh recapture that moves its UV extent across the half-float guard (`_surface_uv_unsafe`), or a material edited
  into or out of a cut-out whose entry already exists. Under hardware that changes the instance class and so the TLAS
  signature; under software it shows only in the uploaded materials. A reader also needs the instance and material
  change signal it already uses.
- It is meant for passes that keep a history of traced results, such as the ray-traced direct shadow pass, which
  needs to know when a shadow ray's answer can change although no light and no instance moved. A reader compares the
  value after `RayScene::update` with the value it saw on its previous frame. Nothing reads it yet.
- Relation to the GI scene epoch (`GIEpochs::SOURCE_SCENE`): `RayScene::update` hashes every ray scene instance's
  final material records, alpha index bits included, into `GISceneTracker` after `_alpha_instance` has run, and when
  `alpha_enabled` is true also hashes `entry.far_state` and `entry.density` for every live slot whose entry has far mode
  1 or 2 (`RayAlphaStore::uses_far`). A surface that starts or stops being traced as a cut-out moves the scene epoch on
  the update where its record changes, and so does a new entry index (cull change, k step, or affine change) or a
  `far_state`/`density` change on a mode 1 or 2 entry. Mode 0 slots add no words, so an all-mode-0 scene hashes as
  before. A layer refilled with new contents while it stays READY (and keeps the same `far_state` and `density`), and
  any table or layer change that leaves every material record and mode 1 or 2 far state as it was, move the serial and
  no epoch. A READY layer that turns PENDING is solid in that frame's trace through the shader's own READY test, and the
  scene epoch follows one update later, when `_alpha_instance` clears the index. The epochs do not read the serial.
- Cost of that relation: every instance of a cut-out surface changes its record when its layer turns READY, is
  evicted or is acquired again. A fill or churn burst (a level load, or more alpha pairs in view than
  `max_alpha_layers`) therefore bumps the scene epoch and publishes a dirty box per instance. Past `dirty_box_cap`
  merged boxes the feed overflows, and ReSTIR GI cuts its history for the frame (`restir_epochs.cpp`,
  `RESTIR_CUT_SCENE_OVERFLOW`) instead of a local reset. Not measured yet.
- The table buffer and the layer texture RIDs can change too (the table grows, the store is reconfigured). The serial
  is not a signal for that: bind them again every frame through `RayScene::append_uniforms`.

## Texture coordinates (`servers/rendering/renderer_rd/environment/ray_scene.cpp`)

- `RayScene::RayScene`: reads `ray_scene/alpha_test` once into `alpha_enabled`. Everything below runs only while it is
  true. With it false nothing is captured, no buffer is made and bindings 9, 10, 13 and 14 are never pushed.
- `RayScene::_capture_surface`: next to the positions, the surface's `ARRAY_TEX_UV` is packed per vertex as one half2
  word (`RayScene::pack_half2`, round to nearest even) into `MeshEntry::uvs`, parallel to `MeshEntry::positions` (a
  vertex without UVs gets 0). `MeshEntry::surface_uv_extent[surface]` keeps the largest absolute UV component in
  floats, for the half-float guard (`_surface_uv_unsafe`). The record is captured for every mesh, opaque ones
  included, because meshes are shared across instances with different materials and materials change at runtime.
- `RayScene::_build_blas`: `MeshEntry::bvh_uvs` holds one `TriangleUv` (four uint32_t words, 16 B: three packed half2
  UVs and a fourth word zero-filled by `TriangleUv`'s default member initialiser) per triangle in the
  same BVH order as `bvh_triangles`. `uv[k]` belongs to the triangle's index `t * 3 + k`, so `uv[1]` sits at
  `v0 + e1` and `uv[2]` at `v0 + e2`.
- `RayScene::_assemble`: appends every mesh's `bvh_uvs` in the order of the `triangles` push, so the record of global
  triangle `i` is entry `i` (binding 9, read with the triangle index both backends already return). The assembly
  vector is local and freed after the upload, so the only kept CPU copy is the per-mesh one. Like the triangle buffer,
  it is uploaded whole on every assembly, and gets one zero record when the scene has no triangles, so the buffer is
  never null after the first assembly while alpha is on.
- `RayScene::get_triangle_uv_buffer`: the buffer ("Ray Scene Triangle UVs"), freed in the destructor.
- Cost: 16 B per triangle on the GPU and once on the CPU, plus 4 B per captured vertex. A scene of 1,427,848
  triangles takes 21.8 MiB on the GPU.

## Material index and live state (`servers/rendering/renderer_rd/environment/ray_scene.cpp`)

- `RayScene::_material_entry`: with alpha on, a material is alpha-tested when its shader declares
  `alpha_scissor_threshold` (StandardMaterial's scissor mode, or any shader that declares it), or declares a bool
  `alpha_test` that is true, with `alpha_threshold`. The threshold comes back in `r_alpha_threshold` (0.5 when the
  value is missing), -1 otherwise. No flag bit is set for it.
- `RayScene::_alpha_entry` (cache miss, alpha-tested and not SKIP) picks the alpha texture and the cutoff by shader
  family:
  - with `use_base_color_opacity_texture` (OpenPBR-style parameters): `base_color_opacity_texture`, cutoff
    threshold / `geometry_opacity`, or through `geometry_opacity_texture_range` with `use_texture_ranges`; UV scale
    and offset from `uv_scale` and `uv_offset`. It stays solid when `base_color_opacity_custom0` is set.
    `use_vertex_alpha` is ignored and counted.
  - with `has_albedo`: `albedo_tex`, cutoff threshold / `mat_alpha`.
  - otherwise (StandardMaterial): `texture_albedo`, cutoff threshold / `albedo.a`.
  - A texture range narrower than 1e-6, a `geometry_opacity`, `mat_alpha` or `albedo.a` of 0 or below, a missing
    `albedo`, or a missing texture keeps the surface solid.
  - The cutoff is `ceil(clamp(cutoff, 0, 1) * 255)`. 0 means every texel passes, so the surface stays solid.
  - The key's cull is the material's `RSE::CullMode`, read once in `_material_entry` (the call that also sets
    `MATERIAL_FLAG_TWO_SIDED` for DISABLED), returned in `r_cull` (DISABLED for a null material) and passed to
    `_alpha_entry` as `p_cull`. `material_get_cull_mode` reads DISABLED until the shader has data, so a cull_back
    cut-out moves to a new entry once, when its shader compiles.
  - The store's entry index goes into the material flags at bit 8 (`MATERIAL_ALPHA_SHIFT`, mask
    `MATERIAL_ALPHA_MASK` = 0xFFFFFF00). Skipped materials are not given a layer, since every ray passes them anyway.
- `RayScene::CANDIDATE_FLAGS` includes `MATERIAL_ALPHA_MASK`: a non-zero index is the eligibility test, so no new flag
  bit is used and bits 5 to 7 stay free. With alpha off no material has a bit above bit 4, so `surface_candidates`
  gives the same answers.
- Split against class. The BLAS split comes from `cached->entry.flags` (`material_candidates`), which carries the
  index whether or not the layer is filled, so a mesh first seen with a cut-out is split from the start and is never
  rebuilt for it. The per-instance record goes through `RayScene::_alpha_instance`, which clears the index when the
  surface's half-float UVs are too coarse (`_surface_uv_unsafe`) or the entry is not READY (`_alpha_live`). The
  instance class (`candidates`) follows that record, so an instance whose only cut-outs are pending stays OPAQUE.
- `RayScene::_surface_uv_unsafe`: half floats have 11 significant bits, so UVs up to extent `e` are spaced
  `2^(floor(log2 e) - 10)`. The surface is unsafe when that spacing times the entry's largest UV scale is larger than
  one layer texel, or when `e` is at or past 65504. With scale 1 the first unsafe extent is 8 at 256 and 4 at 512.
  The strip clears the whole index, so an unsafe surface is not a candidate either.
- `RayScene::alpha_uv_unsafe(extent, scale, layer_size)` holds that math as a static that needs no device (tested by
  `Half float UV guard known answers` in `tests/servers/rendering/test_ray_scene_alpha.cpp`). `_surface_uv_unsafe`
  keeps its own past-the-range test before it reads the table entry, then calls it. `RayScene::float_to_half` and
  `RayScene::pack_half2` are public statics for the same reason (tested by `Half float known answers` and `Every half
  float survives a round trip through float`).
- `RayScene::update`: `RayAlphaStore::configure` and `begin_update` run before the instance loop and
  `RayAlphaStore::update` right after it, so a table entry appended by this frame's `acquire` is uploaded before this
  frame's trace. The per-instance records use the entry state the previous store update left, so a layer filled in
  frame N is traced from frame N + 1. Under hardware, a READY change can change `Instance::pad` (the instance
  class), so the signature changes and the TLAS is rebuilt. The software instance record has no class, so software
  never rebuilds for it. The shader keeps its own READY test as well.
- The anchor snaps down to a 64 m grid (`ANCHOR_GRID`) from the camera position. When it moves, the TLAS order is
  rebuilt and the instances are uploaded again, with every transform relative to the new anchor.
- `RayScene::tlas_rebuilds` counts every `update` that rebuilds the TLAS order and uploads the instances again (any
  backend). `RayScene::hw_tlas_builds` counts every hardware TLAS build that `_hw_update` asks for and
  `RayAccel::update_tlas` performs, which `tlas_rebuilds` does not see.
- `RayScene::_print_tlas_rebuilds` prints `RAYSCENE_TLAS rebuilds N hw_builds M` whatever `alpha_test` says. It prints
  the first change at once, then at most once a second while the totals move, and once more from the destructor with
  ` final` when anything went unprinted. `hw_builds` stays 0 under software.
- `RayScene::_print_alpha`: every update, right after the store update, when any count changed, prints
  `Ray scene alpha:` with the cut-out materials, the solid reasons, surfaces live, pending and unsafe, layers ready of
  used, table entries, the layers released and evicted so far, `far_invalid`, and `entries_retired` (for gate G-CHURN). The store's own `layers_refused` resets in
  `begin_update`; refusals are counted here, per material, as `store full`. That count also takes a material whose
  texture is already freed and one with a scale or offset that is not finite, since `acquire` returns 0 for both; the
  store's `dead_sources_refused` holds the first on its own. It does not wait for a TLAS rebuild: a software READY
  change touches no instance record.
- `RayAlphaStore::get_entry_count` bounds the index in `_alpha_live`.

## Software accept rule (`servers/rendering/renderer_rd/shaders/environment/ray_scene_inc.glsl`)

- Under `RAY_SCENE_ALPHA` the include declares binding 9 (`ray_scene_triangle_uvs`, four uint words per global
  triangle: three packed half2 UVs and a fourth word held at 0), binding 10 (`ray_scene_alpha_mean`, the 128 squared
  8-mip mean array, unused until the far term), binding 13 (`ray_scene_alpha_table`, the store's 56 B entries) and
  binding 14 (`ray_scene_alpha_layers`, the layer array).
- `ray_scene_alpha_passes`: index = flags >> 8. Index 0 and a table entry that is not READY pass (solid). The
  7-argument form (`..., bool p_backface, bool p_shadow`, the software site) then returns false (the ray passes
  through) when `ray_scene_alpha_culled` culls the face; the 5-argument form (the hardware sites until they move)
  has no cull. Both then call `ray_scene_alpha_covered`: the
  UV is `uv0 * (1 - u - v) + uv1 * u + uv2 * v`, then `* scale + offset`, and the coverage is `textureLod` at level 0
  (the array has one mip). Threshold mode (`RAY_SCENE_ALPHA_THRESHOLD`) passes at coverage >= 0.5. Stochastic mode
  passes when coverage > `(hash(hash(seed ^ triangle * 0x9e3779b9) + instance) >> 8) / 2^24`.
- Seed: the caller's per-ray seed after `RAY_SCENE_COVERAGE_SEED`, the same value the BLEND coverage test gets, mixed
  with the triangle and the instance, so two instances of one mesh draw separately. The alpha hash differs from the
  BLEND hash, so a surface that is both BLEND and cut-out draws two streams.
- `ray_scene_trace_blas`: the test sits after the skip mask and before the band test and the tie limit, before
  anything writes `r_hit`. A rejected cut-out never becomes the hit and never changes `r_hit.blend`, so it cannot move
  the tie band either. It passes `det > 0.0` (the hit's `backface`) and the trace's `p_shadow`, never `p_any`.

## Entry key and cull

### `ray_scene_alpha.h:RayAlphaStore::EntryKey`

- 40 B, no padding, hashed as raw bytes: `scale[2]`, `offset[2]`, `bits`, `far_gain` (k), `far_level` (L),
  `far_veil_alpha` (a_v), `far_affine[2]` (scale, bias of the material's alpha transform).
- `bits`: bits 0-1 the cull as `RSE::CullMode` (0 disabled, 1 front, 2 back; `ENTRY_CULL_MASK`), bits 2-3 the far
  mode 0, 1 or 2 (`ENTRY_FAR_MODE_SHIFT`, `ENTRY_FAR_MODE_MASK`), bits 4-31 zero.

### `ray_scene_alpha.cpp:RayAlphaStore::canonical_key`

- Pure static, no device and no `MaterialStorage`. `acquire` calls it right after its refusal checks and uses only
  its result for the hash, the lookup and a new entry.
- A cull that is not 1 or 2 becomes 0. A mode that is not 1 or 2 becomes 0. Bits 4-31 are cleared.
- Mode 1 or 2 with any far float not finite: the key becomes mode 0 and `r_far_invalid` is set (`acquire` counts it
  in `Stats::far_invalid`). A refusal would return index 0 and trace the slot solid, losing a hard cut-out that has
  nothing wrong with it; mode 0 keeps the hard test and drops only the far term.
- Mode 1 or 2: k = max(k, 0), a_v = clamp(a_v, 0, 1). Mode 0: k, L, a_v and the affine are 0, so mode 0 keys that
  differ only in unused far values share one entry.
- The far floats then map -0.0 to +0.0 (`if (v == 0.0f) { v = 0.0f; }`; `+ 0.0f` may be dropped by a compiler). Scale
  and offset are left as they come, so the entries and indices of keys with bits 0 are allocated as before.
- Every key float is then finite, so the float `==` equality is reflexive and a key that held a NaN far value finds
  its entry again next frame. Without this, a NaN would never equal itself and `acquire` would allocate an entry every
  frame.

### `ray_scene_alpha.cpp:_entry_hash`, `_same_key`

- `_entry_hash(layer, key)`: murmur3 over the 40 key bytes, seeded with the layer, layer in bits 32-63. Internal to
  `entry_map`.
- `_same_key`: equality on every key field (float `==`), next to the layer and generation test.

### `ray_scene_alpha.h:RayAlphaStore::TableEntry` and `ray_scene_inc.glsl:RaySceneAlphaEntry`

- 56 B, std430 array stride 56 (struct alignment 8 from the `vec2` members). Offsets: `layer` 0, `state` 4,
  `scale` 8, `offset` 16, `bits` 24, `far_gain` 28, `far_level` 32, `far_veil_alpha` 36, `far_affine` 40,
  `density` 48, `far_state` 52. Static asserts after the class pin the size and the offsets of `bits`, `far_affine`,
  `density` and `far_state`.
- Every member has a zero default, and `acquire` builds `TableEntry entry = {}`, so the uploaded bytes are defined.
- `acquire` writes the key fields (`scale`, `offset`, `bits`, the far values) into a new entry, and publishes `density`
  and `far_state` for mode 1 or 2 entries through `_publish_far`. `begin_update` refreshes `density` and `far_state`
  for live mode 1 or 2 entries.

### `ray_scene_inc.glsl:ray_scene_alpha_culled`

- Rule: a ray stops only on the faces raster draws. For a ray with `p_shadow` false the viewer is the ray's origin.
  For a shadow ray (`p_shadow` true, set only by `ray_scene_unshadowed`) the viewer is the light: the ray runs from
  the receiver toward the light, so the light sees the opposite face, `seen_back = p_backface != p_shadow`. BACK culls
  `seen_back`, FRONT culls `!seen_back`, cull 0 culls nothing.
- `RAY_SCENE_MATERIAL_SHADOW_TWO_SIDED` (bit 5, 32) skips the cull for shadow rays only (a DOUBLE_SIDED caster).
  Defined inside `RAY_SCENE_ALPHA` with the cull defines.
- `p_backface` is the object-space sign of det, so mirrored instances need no determinant term: with l =
  world-to-object, d_obj . n_obj = d . (l^T n), the transformed normal whose meaning raster's REVERSED variant keeps.
  A world-space winding test would be wrong exactly when det(l) < 0.
- Index 0 (opaque) and an entry that is not READY return before the cull, so opaque and pending surfaces keep both
  faces.
- With `alpha_test` off nothing of this is compiled: every cull line is inside `#ifdef RAY_SCENE_ALPHA`.

## Double-sided caster flag

### `ray_scene.h:RayScene::MATERIAL_FLAG_SHADOW_TWO_SIDED`

- Material bit 5 (32), the C++ twin of `RAY_SCENE_MATERIAL_SHADOW_TWO_SIDED`. Raster's shadow passes draw a
  `SHADOW_CASTING_SETTING_DOUBLE_SIDED` instance with the DOUBLE_SIDED pipeline variant, so both faces cast. The bit
  makes shadow rays match: `ray_scene_alpha_culled` skips the cull when `p_shadow` is true and the bit is set. GI and
  non-light any-hit rays (`p_shadow` false) ignore it.
- Not in `CANDIDATE_FLAGS`: a slot that carries it is already a candidate through its alpha index, so the hardware
  class and split are unchanged. Not in `RAY_SCENE_SKIP_MASK`: it never skips a hit.
- Per-instance like SHADOW_PASS and SHADOW_ONLY, because the `Instance` word has no free bit (`pad` holds the hardware
  class and the remap region).

### `ray_scene.cpp:RayScene::shadow_cast_flags`

- `SHADOW_CASTING_SETTING_DOUBLE_SIDED` gives `MATERIAL_FLAG_SHADOW_TWO_SIDED`, with or without the occluder opt-in.
  `RayScene::update` reads the setting from `geometry->data->cast_shadows` every frame, as for the other cast flags.

### `ray_scene.h:RayScene::slot_cast_flags`

- The cast flags one material slot receives in `RayScene::update`: all of them when the slot's alpha bits are still
  set after `_alpha_instance`, that is a live cut-out, and all but bit 5 otherwise. Opaque, pending, unsafe-UV and
  refused slots keep the C-2 bytes, and with `alpha_test` off no slot has alpha bits, so bit 5 is never set.
- The slot bytes go into the scene hash and the feed state, so a cast-setting change on an instance with a live
  cut-out moves SOURCE_SCENE once, as a SHADOW_PASS change does. A slot that turns pending loses bit 5 in the same
  frame it loses its alpha bits.
- Tests: `test_ray_scene_cast.cpp` pins the DOUBLE_SIDED known answers (`Cast setting flag known answers`), the role
  stop table (`Shadow and bounce rays per cast setting`) and the slot rule for live and solid slots plus the
  non-candidate check (`The double sided caster bit goes only onto live cut-out slots`). `test_ray_scene_alpha.cpp`
  pins the quad shadow, M1 and non-light rows (`Shadow rays and DOUBLE_SIDED on the cut-out quad`).
  `test_ray_scene_class.cpp` sweeps 64 low flag combinations in `A surface is opaque to hardware only when every
  software ray accepts it outright`, so bit 5 is in the opaque-class rule.

## Binding 9 stride

### `ray_scene.h:RayScene::TriangleUv`

- Four `uint32_t` words (`uint32_t uv[4] = {};`), 16 B (`static_assert(sizeof(TriangleUv) == 16)`): `uv[0..2]` hold the
  packed half2 vertex UVs and `uv[3]` stays 0 until the far term populates it.
- The default member initialiser makes `TriangleUv` not trivially default-constructible while keeping it trivially
  copyable and an aggregate. `LocalVector::resize` in `RayScene::_build_blas` therefore takes its initialising path and
  zero-fills every record (`uv[3]` included), so `_build_blas` writes `uv[0..2]` only and leaves `uv[3]` deterministic
  zero. `RayScene::_assemble` sizes and uploads the buffer through `sizeof(TriangleUv)` and zero-fills fallback and
  empty records with `TriangleUv()`.

### `ray_scene_inc.glsl:ray_scene_alpha_covered`

- Indexes binding 9 (`ray_scene_triangle_uvs`, `std430` `uint data[]`) at `base = p_triangle * 4u` and reads `base`,
  `base + 1u` and `base + 2u`. Both software and hardware alpha tests go through `ray_scene_alpha_covered`.

## Far values and churn gate

### `ray_scene.cpp:RayScene::_alpha_entry`

- Reads the material's raw far parameters without clamping (`RayAlphaStore::canonical_key` handles clamping, `-0.0f` to
  `+0.0f` mapping, and non-finite fallback to mode 0 counted in `Stats::far_invalid`):
  - `alpha_far_mode` as `Variant::INT` (missing or non-int defaults to 0). Only modes 1 and 2 set
    `key.bits |= uint32_t(far_mode) << RayAlphaStore::ENTRY_FAR_MODE_SHIFT` and read `alpha_far_gain` (k),
    `alpha_far_level` (L) and `alpha_far_veil_alpha` (a_v) with `_param_float(..., 0.0f)`; other modes (including
    probe-only 3 and 4) stay at mode 0 so mode 0 materials skip the extra parameter lookups.
  - `far_affine` (scale, bias) comes from the branch that computes the cutoff: `(span, range.x)` for range materials,
    and `(opacity, 0)`, `(mat_alpha, 0)` or `(albedo.a, 0)` otherwise (`canonical_key` zeroes it when mode is 0).
- Invariant: `k` (`alpha_far_gain`) never changes per frame through the entry key. A probe may step `k` between settled
  readings (one new entry and one `SOURCE_SCENE` bump per step); a live per-frame `k` knob goes in a uniform, never in
  the key.

### `ray_scene_alpha.cpp:RayAlphaStore::_publish_far`, `acquire`, `begin_update`

- `RayAlphaStore::uses_far(p_bits)` is true only for canonical mode 1 or 2 (`(p_bits >> ENTRY_FAR_MODE_SHIFT) &
  ENTRY_FAR_MODE_MASK`).
- `_layer_density(width, height)` returns `0.0f` when `width == 0` or `height == 0` (avoiding `-inf` before the first
  source format read in `update()`), else `0.5f * (Math::log2(float(width)) + Math::log2(float(height)))` (avoiding
  `uint32_t` product overflow at 65536 x 65536). Mode 0 entries keep `density` at `0.0f`.
- `_publish_far` writes `density` and `far_state` (`LAYER_READY` when `layer.state == LAYER_READY &&
  layer.far_filled_signature == layer.filled_signature`, else `LAYER_EMPTY`) in two places only, both before
  `RayScene::update`'s scene hash walk: in `acquire` for a new mode 1 or 2 entry, and in `begin_update` for every
  surviving live mode 1 or 2 entry after the retire check, calling `_mark_state_changed()` when either changes.
  `update()` and `sync_entries()` never write `density` or `far_state`, so table uploads and scene epoch updates stay
  on the same frame.

## Mean alpha array

### `ray_scene_alpha.cpp:RayAlphaStore::_ensure_texture`, `_free_textures`, `release`

- Creates `mean_texture` in the same call as `layer_texture`: `DATA_FORMAT_R8_UNORM`, `TEXTURE_TYPE_2D_ARRAY`,
  `128 x 128`, `array_layers = max_layers`, `mipmaps = 8` (128 down to 1: 21,845 B per layer, 11,184,640 B at 512
  layers), usage `SAMPLING_BIT | STORAGE_BIT`.
- Also creates 8 storage slice views `mean_mip_views[0..7]` through `texture_create_shared_from_slice` with
  `TEXTURE_SLICE_2D_ARRAY` and `max_layers` layers, because each compute image binding targets one mip. If the layer
  texture, mean texture or any slice view fails, `_free_textures` frees everything created so far and `_ensure_texture`
  returns false.
- `_free_textures` frees `mean_mip_views` before `mean_texture`, then `layer_texture`, and checks each RID for validity
  before calling `RD::get_singleton()` so device-free tests (such as `Alpha store keys, entries and clamps need no
  device` and `Alpha change serial stays still when nothing a ray reads changes` in
  `tests/servers/rendering/test_ray_scene_alpha.cpp`) never touch a null `RenderingDevice`. `release` calls
  `_free_textures` and also frees `mean_mip_pipeline`.
- `RayScene::_alpha_ready` (`ray_scene.cpp`) also requires `alpha_store.get_mean_texture().is_valid()`, so `is_ready()`
  is never true while binding 10 would receive a null RID.

### `ray_scene_alpha_fill.glsl:main` and `ray_scene_alpha.cpp:RayAlphaStore::_ensure_pipeline`

- Second mode in the existing fill shader (`#define MODE_MEAN_MIP`, compiled as shader variant 1 into
  `mean_mip_pipeline`) instead of a separate shader file: avoids any SCons/build-list change, reuses `fill_shader`,
  `fill_version` and the 32 B `FillParams` push-constant block (`layer` and `layer_size` as the destination mip width),
  and keeps both kernels in one file.
- Default mode writes hard coverage to binding 1 (`alpha_layers`) and mean mip 0 to binding 2 (`mean_layers`, bound to
  `mean_mip_views[0]`) on every fill dispatch, regardless of entry modes, at zero extra source reads. The early
  out-of-bounds return is removed because the dispatch is exact (`layer_size / 8` groups for 256 or 512) and a
  workgroup `memoryBarrierShared(); barrier();` follows. Each thread stores its raw alpha sum and sample count in
  `shared` 8x8 arrays; one thread per `block_size x block_size` block (2x2 at 256, 4x4 at 512, both dividing 8) writes
  `block_sum / float(block_count)` for an exact unweighted box mean even on unequal source blocks. The hard cut-share
  formula is untouched (contract 7b item 5 row 1).
- `MODE_MEAN_MIP` reads a 2x2 block from binding 0 (`source_mip`, `readonly r8 image2DArray`) and writes its 0.25-scaled
  sum to binding 1 (`dest_mip`, `writeonly r8 image2DArray`), guarding `id.x >= params.layer_size || id.y >=
  params.layer_size` for mips 5 to 7 (4x4, 2x2, 1x1).

### `ray_scene_alpha.cpp:RayAlphaStore::acquire`, `begin_update`, `layer_needs_far`, `commit_far`, `_update_far_mips`

- `Layer` gains `bool far_wanted = false` and `uint64_t far_filled_signature = 0` (both reset by `layer = Layer()` in
  `_release_layer`). `begin_update` clears `far_wanted` on every layer before the walk (regardless of whether
  `liveness` is set). `acquire` sets `layer.far_wanted = true` whenever `uses_far(key.bits)` is true, next to
  `layer.last_used = frame` and before the `entry_map` lookup, so the found-entry path sets it too.
- `layer_needs_far(p_layer)` (pure, no device): true when `source.is_valid() && state == LAYER_READY && far_wanted &&
  far_filled_signature != filled_signature`. This catches a READY layer whose first mode 1 or 2 entry arrives later
  (the case the hard-fill check skips).
- `_update_far_mips()` runs in `update()` after the hard fill list ends and before `sync_entries()`. It collects layers
  with `layer_needs_far(i)` in layer order (including layers just filled in the same `update()`), charging
  `MEAN_MIP_READS = 21,844` reads per layer against `texels_per_frame` (stopping once the frame has done work and the
  next layer would exceed the budget). When no layer wants far mips, nothing is dispatched. Otherwise it binds
  `mean_mip_pipeline` in one compute list and dispatches mips 1 to 7 level by level across all chosen layers with
  `compute_list_add_barrier` between levels, then calls `commit_far(index, layers[index].filled_signature)` and adds
  the read count to `stats.fill_texels_this_frame` and `stats.fill_texels_total`.
- `commit_far(p_layer, p_signature)` (pure, no device): sets `far_filled_signature = p_signature` when `source` is
  valid, `state == LAYER_READY`, and `filled_signature == p_signature`. It does not touch `TableEntry::far_state` or
  `change_serial`; `begin_update` of the next frame publishes `far_state = LAYER_READY` and marks the state change so
  the GPU table upload and the scene hash move on the same frame.
- Known gap: a READY layer can be refilled with new contents (`commit_fill`) in a frame whose fill budget is already
  spent, so `_update_far_mips` defers its mips. Its entries keep `far_state = LAYER_READY` until the next
  `begin_update`, so for that frame mip 0 of the mean array is new while mips 1 to 7 are from the old contents. No ray
  reads binding 10 yet. A far-term reader must compare the layer's `far_filled_signature` or accept one stale frame.

### `ray_scene_backend.cpp:RayScene::append_uniforms` and `ray_scene_inc.glsl:ray_scene_alpha_mean`

- Binding 10 (`RAY_SCENE_SET`, `UNIFORM_TYPE_SAMPLER_WITH_TEXTURE`) binds `alpha_store.get_mean_texture()` with the
  default `CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS`, `CANVAS_ITEM_TEXTURE_REPEAT_ENABLED` sampler inside the
  `alpha_enabled` branch right after binding 9, matching `layout(set = RAY_SCENE_SET, binding = 10) uniform
  sampler2DArray ray_scene_alpha_mean;` inside `#ifdef RAY_SCENE_ALPHA`.
- Both `DDGI::_trace` (`ddgi.cpp`) and `RestirGiCore::_dispatch` / `_dispatch_dump`
  (`modules/gi_restir/restir_gi_core.cpp`) build set 1 exclusively through `RayScene::append_uniforms`, so every
  shader compiled with `RAY_SCENE_ALPHA` receives binding 10. No ray samples binding 10 in this commit.

### `ray_scene.h:RayScene::AlphaCounts::entries_retired` and `ray_scene.cpp:RayScene::_print_alpha`

- `AlphaCounts::entries_retired` (`uint32_t`, keeping `AlphaCounts` padding-free for `memcmp`) is copied from
  `uint32_t(stats.entries_retired_total)` (incremented in `RayAlphaStore::_retire_entry`) beside `layers_released` and
  `layers_evicted`, and printed on the `Ray scene alpha:` line as `, %d entries retired.`.
- Gate **G-CHURN**: on every 8b and identity leg, after settle, `entries_retired` and `table_entries` on the
  `Ray scene alpha:` line stay constant over 100 frames and `GIEpochs::SOURCE_SCENE` does not move.

## Scene epoch, DDGI and no per-frame churn

### `ray_scene.cpp:RayScene::update`

- After hashing the instance's `Material` records into `scene_hash`, when `alpha_enabled` is true, the walk loops over
  the instance's surface slots: for every slot whose final alpha index (`flags >> MATERIAL_ALPHA_SHIFT`) is non-zero
  and whose `TableEntry` has far mode 1 or 2 (`RayAlphaStore::uses_far(entry.bits)`), it hashes `entry.far_state`
  (`hash_murmur3_one_32`) and `entry.density` (`hash_murmur3_one_float`).
- The key's far words (`far_mode`, `far_gain`, `far_level`, `far_veil_alpha`, `far_affine`) are already covered
  indirectly by `Material::flags`, because any change to a key field allocates a new entry index in `acquire` while the
  old entry remains live for one frame (`last_used + 1 < frame`). Only `far_state` and `density` mutate in place on a
  live entry.
- Static-source `density` and `far_state` retain their `begin_update`/`acquire` publication timing. For externally
  writable sources, `RayAlphaStore::sync_entries` publishes the current observation after fill scheduling. The scene
  hash is assembled after `alpha_store.update`, so a far readiness or density change reaches `GIEpochs::SOURCE_SCENE`
  in the same update that publishes the table used by rendering.
- `RayScene::update` adds no alpha words for disabled alpha or index 0. Each ready alpha layer also contributes its fill
  revision, including far mode 0, so changes to near coverage invalidate the scene epoch. Pending layers contribute no
  fill revision. Externally writable sources can advance the epoch each time they are serviced even if pixels are unchanged.
- DDGI does not read `GIEpochs::SOURCE_SCENE` and keys its moved-instance reset (`RayScene::collect_moved_instances`) on
  `instance_signature` (which hashes `Instance` records only, not `Material` flags or alpha table fields), so neither
  DDGI resets nor TLAS rebuilds are triggered by cull, cast-bit, far-key, `density`, or `far_state` transitions.
- The dirty-box feed (`feed_state`, `feed_surface_state`) hashes final `Material::flags` and `coverage` (covering
  entry index changes, cull/k changes, cast-setting changes, and READY/PENDING transitions) and does not hash
  `far_state` or `density` separately.

### `ray_scene.cpp:RayScene::_material_entry`, `_alpha_entry` (cull-mode compile one-shot)

- `MaterialStorage::material_get_cull_mode` returns `CULL_MODE_DISABLED` (0) while a material's shader has no compiled
  shader data, then returns the declared cull mode (e.g. `CULL_MODE_BACK`, 2) once compiled. Because `material_cache`
  is rebuilt each frame, a `cull_back` cut-out acquires a cull-0 entry until compile, then acquires one new cull-2
  entry on the compile frame (one `SOURCE_SCENE` bump), and the cull-0 entry retires on the following frame via
  `last_used + 1 < frame` (re-uploading the table without changing slot flags or causing a second epoch bump). Once
  compiled, the key and entry index are constant across frames.

## Bindings and defines (`servers/rendering/renderer_rd/environment/ray_scene_backend.cpp`)

- `RayScene::append_uniforms`: with `alpha_enabled`, pushes bindings 9 (`triangle_uv_buffer`), 10 (`mean_texture` with
  the default `LINEAR_WITH_MIPMAPS`, `REPEAT_ENABLED` sampler), 13 (the table) and 14 (the layer array with the default
  `LINEAR`, `REPEAT_ENABLED` sampler) for both backends. Every consumer of the include binds its ray scene set through
  this call.
- `RayScene::alpha_shader_defines` (`ray_scene.cpp`): `RAY_SCENE_ALPHA`, plus `RAY_SCENE_ALPHA_THRESHOLD` for mode 1,
  only when the ray scene singleton has `alpha_enabled`. The define and the bindings follow the same flag. A shader
  with the define and no bindings fails to bind; one with the bindings and no define traces every cut-out solid with
  no error.
- `servers/rendering/renderer_rd/environment/ddgi.cpp:DDGI::init`: `trace_defines` ends with
  `RayScene::alpha_shader_defines()`. `_ensure_trace_hw` builds on `trace_defines`, so both DDGI trace shaders (all
  modes, surfel trace included) get it. With alpha off the string is empty.
- `modules/gi_restir/restir_gi_core.cpp:RestirGiCore::_defines`: the software and hardware ray variants add
  `RayScene::alpha_shader_defines()` to their defines; `VARIANT_NONE` gets no defines. The module builds its shaders on
  first use, after `RendererCompositorRD` has made the ray scene, and binds its ray scene set through
  `append_uniforms`. In Stochastic mode every sample carries the seed of the ray that made it (RTGI's seed at that
  pixel, or SOURCE's own), and every re-test passes it: the visibility rays (`rs_visible`), the fresh-Lo rays
  (`rs_fresh_lo`, FINAL and VALIDATE) and, with `gi/replay_seed`, the replayed rays. On the same triangle and instance
  index a stochastic cut-out then compares its coverage with the same random value as the ray that made the sample,
  so a sample that stopped on a partly covered texel is found again and edges are not darkened by the re-test
  (`docs/code-notes/restir-gi.md`, `restir_gi.glsl:RS_ALPHA_SEED`). Threshold mode needs no seed for the cut-out; the BLEND coverage draw (`RAY_SCENE_COVERAGE_MISSES`) still uses
  the seed in every mode, so with alpha off or in Threshold mode a module re-test of a sample that stopped on a
  partly covered BLEND surface is still a new draw. Threshold for the
  module's variants alone would not do it, because the `TRACE_MODE_RTGI` ray that makes the sample stays
  stochastic, so its re-tests would disagree on partly covered texels. The module reads the dirty-box feed (`FEED_MODULE`), whose state
  word hashes the final material flags, alpha index bits included (`RayScene::feed_surface_state`): a surface that
  starts or stops being a cut-out publishes a box, a READY layer refilled with new contents does not.
- `servers/rendering/renderer_rd/environment/direct_shadows.cpp`: its `defines` do not include
  `RayScene::alpha_shader_defines()`. It still binds its ray scene set through `append_uniforms`, so the alpha
  bindings are pushed and its shader does not declare them. To apply cut-outs there, add the call to its defines;
  its history would then also need the change serial.
- `RayScene::is_ready`: with alpha on it also needs the UV buffer, the table, the layer array and the mean array
  (`_alpha_ready`). All four exist after the first `update`.

## Hardware accept rule (`servers/rendering/renderer_rd/shaders/environment/ray_scene_hw_inc.glsl`)

- `ray_scene_trace_masked_near` (wrapped by `ray_scene_trace_masked`), candidate loop of both the near and the main pass (see ray-scene-near-backface.md): `ray_scene_alpha_passes` runs after the tie limit reject and before
  the BLEND coverage miss, so a rejected cut-out is never confirmed. Both hardware sites (main candidate loop and tie
  query) still call the 5-argument form, which has no cull: until they move to the 7-argument form, software culls
  cull_back and cull_front cut-outs and hardware does not. It uses the same table, layer, seed
  (`RAY_SCENE_COVERAGE_SEED` of the caller's seed), global triangle (`ray_scene_hw_triangle`) and instance (the custom
  index, which is the software instance index) as the software rule.
- Barycentrics: `rayQueryGetIntersectionBarycentricsEXT(query, false).yx`. The BLAS soup is written v0, v0 + e2,
  v0 + e1 (`ray_accel.cpp:RayAccel::ensure_blas`), so the query's (b1, b2) weigh e2 then e1, which is (v, u) against
  the software (u, v). The swap gives the software UV.
- Tie query: it accepts only BLEND surfaces, and a BLEND surface can carry a cut-out index too. The software band test
  runs after the alpha test, so the tie query applies the same test (with `tie_query`'s barycentrics) to keep the two
  backends on one rule.
- Reaching the loop. In exact mode (all casters, or one of the 7 grouped masks) only candidate instances and the
  candidate geometry of split instances reach it. A live cut-out keeps its index in the per-instance record, so its
  instance is a candidate or split. A pending one is stripped to index 0 and its instance can be OPAQUE, traced solid
  like the software fallback. In non-exact mode (`gl_RayFlagsNoOpaqueEXT`) every triangle reaches the loop, and the
  stripped index plus the shader's READY test keep pending layers solid.
- There is no hardware against software parity harness, so edge texels where the two backends' barycentrics differ
  are not checked by a test.

## Live switch (`ray_scene.cpp:RayScene::update`, `alpha_slot_flags`, `alpha_split_bits`, `hw_split_stale`, `alpha_scene_signature`)

- Latch and generation: `RayScene::RayScene` takes the initial `alpha_enabled` snapshot from
  `rendering/global_illumination/ray_scene/alpha_test` and registers `_alpha_texture_live` on `alpha_store`
  unconditionally. At the start of every `RayScene::update`, right after `_backend_tick()`, `alpha_on` is read with
  `GLOBAL_GET_CACHED` and compared against `alpha_enabled` to set `alpha_edge = alpha_on != alpha_enabled`. On an edge,
  `alpha_enabled` takes `alpha_on` and `alpha_generation` increments (wrapping 0 to 1; exposed by
  `RayScene::get_alpha_generation()`). UV capture/build/upload (`_capture_surface`, `_build_blas`, `_assemble`),
  threshold parsing (`_material_entry`), `RayScene::alpha_shader_defines()` (which now returns empty only when
  `!singleton`), bindings 9, 10, 13 and 14 (`RayScene::append_uniforms`) and `_alpha_ready()` in `RayScene::is_ready()`
  all run unconditionally.
- Masked cache (`ray_scene.h:RayScene::alpha_slot_flags`): `CachedMaterial` keeps `uint32_t alpha_flags = 0` alongside
  `Material entry`. On a `material_cache` miss, `_alpha_entry` runs unconditionally so cut-out layers are acquired,
  then `entry.alpha_flags = entry.entry.flags & MATERIAL_ALPHA_MASK` saves the raw index bits and
  `entry.entry.flags = alpha_slot_flags(entry.entry.flags, alpha_enabled)` clears `MATERIAL_ALPHA_MASK` whenever
  `alpha_enabled` is false. Everything downstream of the cache (`materials`, `_alpha_instance`, `slot_cast_flags`,
  `feed_surface_state`, `surface_candidates`, `material_candidates`, `candidates`, the uploaded material bytes and the
  material hash) therefore sees exact legacy-off flags while off, and the far-state hash loop remains gated on
  `alpha_enabled`.
- Store warming while off: `alpha_store.configure`, `alpha_store.begin_update`, `_alpha_entry` (`alpha_store.acquire`),
  and `alpha_store.update()` run unconditionally on every `RayScene::update`. Layers and mean mips keep
  acquiring, stamping and filling while `alpha_test` is off, so turning it on traces already-`READY` layers on the next
  update instead of waiting for fills. `_print_alpha()` runs only while `alpha_enabled` is true, so an off run's log has
  no `Ray scene alpha:` line, as before the switch (run kits read that line to tell an on leg from an off leg). While
  off, `_alpha_instance` is skipped, so `live_surfaces`, `pending_surfaces` and `solid_unsafe_uv` stay 0.
- Hardware split (`ray_scene.h:RayScene::alpha_split_bits`, `hw_split_stale`): `MeshEntry::hw_alpha_on` records the
  `alpha_enabled` state under which the mesh's frozen hardware split was built. When `accel` is active, each instance
  accumulates `alpha_split |= alpha_split_bits(s, cached->entry.flags, cached->alpha_flags)` across its surfaces, where
  `alpha_split_bits` computes `surface_candidates(s, flags | alpha_flags) & ~surface_candidates(s, flags & ~MATERIAL_ALPHA_MASK)`:
  non-zero only for surfaces that are hardware candidates solely because of their cut-out index (not already `SKIP`,
  `BLEND`, `SHADOW_PASS` or `SHADOW_ONLY`). Before `_hw_instance`, `hw_split_stale(mesh->hw_generation, mesh->hw_alpha_on,
  alpha_enabled, alpha_split)` is true only when `hw_generation != 0 && hw_alpha_on != alpha_enabled && alpha_split != 0`;
  when stale, `mesh->hw_generation` and `mesh->hw_region` are cleared to 0. If `mesh->hw_generation == 0` entering
  `_hw_instance` (`hw_fresh`), `_hw_instance` assigns a fresh `hw_mesh_generation`, recomputes the split for the current
  `alpha_enabled` state and triggers `RayAccel::ensure_blas` to rebuild the BLAS, after which `mesh->hw_alpha_on` is
  recorded as `alpha_enabled`. Only meshes whose candidacy comes solely from a cut-out are rebuilt, once per toggle, and
  any orphaned remap entries are dropped at the next `_hw_assembled()`. Because `RayAccel::ensure_blas` caps BLAS builds
  per frame, a large toggle can take several frames during which an unbuilt instance is skipped (`hw_usable == 0`), as
  for any new mesh.
- Scene epoch salt (`ray_scene.h:RayScene::alpha_scene_signature`, `ALPHA_ON_SALT`): defined inline in `ray_scene.h`
  (`ALPHA_ON_SALT = 0x9e3779b9u`, since `ray_scene.h` already includes `gi_epochs.h`). Before
  `GIEpochs::get_singleton()->apply(scene_tracker.update(scene_signature, scene_members))`, `scene_signature` passes
  through `alpha_scene_signature(scene_signature, alpha_enabled)`, which returns `p_signature` unchanged when off
  (legacy identity) and `GISceneTracker::add_instance(p_signature, ALPHA_ON_SALT)` when on. A toggle therefore changes
  the scene signature once even when no material slot changes, advancing `GIEpochs::SOURCE_SCENE` exactly once (the
  dirty feed's own bump in `ray_scene_dirty.cpp:_feed_end` is skipped once the epoch has already moved).
- Dirty-box feed overflow (`ray_scene_dirty.cpp:RayScene::_feed_end`): when the feed is active, `overflow` is computed
  as `feed_frame_overflow(feed_raw_overflow, feed_baseline) || alpha_edge`. On the toggle frame the feed publishes
  overflow (without changing anything else in the feed).
- DDGI and surfel history (`ddgi.h`, `ddgi.cpp:DDGI::update`, `surfel_gi.h`, `surfel_gi.cpp:SurfelGI::update`):
  `DDGI::Volume` and `SurfelGI::SurfelVolume` each store `uint32_t alpha_generation = 0`. When `RayScene::get_singleton()`
  exists and `volume->alpha_generation != ray_scene->get_alpha_generation()`, `update` re-instantiates and reallocates
  the volume and stores the new generation (0 if there is no singleton). Reallocation clears the probe/surfel history
  buffers and restarts the update schedule.
- RT GI follow-up (not in this commit): per-pixel RTGI (`rtgi.cpp`) and ReSTIR GI (`modules/gi_restir`) do not read
  `RayScene::get_alpha_generation()` yet (planned follow-up: a history cut like `CUT_BACKEND` in RTGI, and
  `clear_history` with no `RESTIR_F_HISTORY` in ReSTIR GI). Until then, ReSTIR GI cuts its history on the edge frame
  through the feed overflow (`restir_epochs.cpp`, `SCENE_OVERFLOW`), while per-pixel RTGI keeps its history across a
  toggle. `direct_shadows.cpp` is unchanged (no cut-outs there yet).
- Resident memory while off: because UVs and the alpha store are always allocated, turning `alpha_test` off keeps the
  layer array (32 MiB at the default 512 x 256 R8; 128 MiB at 512 x 512), the mean array (about 11 MB: 11,184,640 B at
  512 layers), 56 B per table entry and 16 B per triangle of UVs (GPU and CPU `bvh_uvs`, plus 4 B per captured vertex)
  resident.

## Unit tests (`tests/servers/rendering/test_ray_scene_alpha.cpp`, `test_ray_scene_cast.cpp`, `test_ray_scene_class.cpp`)

None of these tests needs a RenderingDevice. They call `RayScene` statics, a `RayAlphaStore` that is configured but
never updated, and CPU copies of the shader rules. `RayAlphaStore::update` and `RayScene::update` are not reached.

- `Half float known answers`: `RayScene::float_to_half` and `RayScene::pack_half2` against IEEE binary16 values. It
  pins signed zero, 1, -2, 65504, infinities, quiet and signalling NaN (both give 0x7E00 with the sign), the smallest
  subnormal 2^-24, flush to signed zero at 2^-25 and below, round up just above 2^-25, ties to even in the subnormal
  and normal ranges, carries into the exponent (1023.5 x 2^-24 to 0x0400, 2 - 2^-12 to 0x4000) and the x-low,
  y-high pack. The clamp is not IEEE: everything from 65520 up, which IEEE rounds to infinity, gives 0x7BFF.
- `Every half float survives a round trip through float`: every binary16 pattern except NaN maps back to itself.
- `Half float UV guard known answers`: `RayScene::alpha_uv_unsafe`. At scale 1 and layer 256 the spacing equals one
  texel (2^-8) for extents 4 to just under 8, which is still safe because the test is a strict greater-than; extent 8
  is the first unsafe one. Scale 2, or layer size 512, moves that to 4; scale 4 to 2. Extent 0 is safe, and 65504 is
  unsafe at any scale, 0 included.
- `Alpha store keys, entries and clamps need no device`: `RayAlphaStore::configure` and `acquire` with fake texture
  RIDs. `configure(2, ...)` gives 16 layers, and the 17th new (texture, cutoff) pair is refused with index 0 and one
  `layers_refused`; a full store still adds entries on known layers. Null RID, cutoff 0 and cutoff 256 give 0. The
  same key returns the same index, a new scale or offset adds an entry on the same layer, a new cutoff or texture adds
  a layer, and every entry stays EMPTY without a fill. A reconfigure with the same clamped values keeps the entries; a
  new size starts over.
- The lifetime, serial and far cases below use `test_texture_live`, a liveness function backed by the test's own list of
  live ids, and drive frames with `begin_update`, `acquire`, `commit_fill`, `layer_needs_far`, `commit_far` and
  `sync_entries`.
- `Alpha store keys layers by the whole RID`: two RIDs of slot 7 whose validators differ by 2^23 give the same
  `(id << 9) | 128` (checked) and two different layers.
- `Alpha store refuses a texture that is already freed`: index 0, no layer, no entry, one `dead_sources_refused`,
  reset by the next `begin_update`.
- `Alpha store releases the layers of freed textures`: 4 layers, 2 textures freed. The next `begin_update` leaves 2
  live layers of 4 slots, generation 1 on the freed ones, both entries tombstoned and free. A new texture takes a freed
  slot and a freed entry index, EMPTY, with the slot's new generation.
- `Alpha store resets the old texture's entry before a new texture reuses it`: a READY layer's texture is freed and a
  new texture lands in the same slot at the same scale and offset. It gets the old entry index back from the free
  list, reset: EMPTY until its own fill, with the slot's new generation, and a second acquire returns the same index.
- `Alpha store evicts the least recently used layer at the cap`: 16 layers at frame 1. At frame 2, with 10 of them
  used, a new texture is refused, since the other 6 were used in the frame before. At frame 3 it evicts layer 10, the
  lowest of the 6; a second new texture evicts layer 11 although 11 and 12 were made READY first. The 10 in use keep
  their slots.
- `Alpha store keeps layers used earlier in the same frame`: 16 old layers, then three `begin_update(5)` calls with
  disjoint sets of 8. The first two sets evict the 16 old layers; the third is refused in full, and the first two keep
  their layers.
- `Alpha store ages out entries no material uses`: one texture at a new offset every frame for 1000 frames. No
  refusal, at most 2 live entries, 3 entries in all (the null entry included), 998 retired. NaN and infinite scales
  and offsets return 0 and add no entry.
- `Alpha change serial stays still when nothing a ray reads changes`: 0 on a store that is not configured, through
  `begin_update`, a refused `acquire`, `sync_entries` and `release`, and 0 after the first `configure`. Once a layer
  is READY and settled, eight frames of re-acquiring the same entry, a `commit_fill` with the same signature and
  `sync_entries` leave it still, and so does a `configure` that only changes the texel budget. Also holds a `cull_back`
  mode 1 far key for 100 settled frames after `commit_fill` and `commit_far`, checking constant index, `layer_needs_far`
  false, constant serial and zero retirements.
- `Alpha change serial moves when a layer becomes ready and once more on the next frame`: an EMPTY layer made READY
  through `commit_fill` and `sync_entries` moves it; the next `begin_update` moves it again; the frame after leaves it
  still.
- `Alpha change serial takes the next-frame move only in a later frame`: after a layer is made READY in frame 5, a
  second `begin_update(5)` leaves it still; `begin_update(6)` moves it; a second `begin_update(6)` and
  `begin_update(7)` leave it still.
- `Alpha change serial moves when a ready layer is refilled`: `commit_fill` with a new signature on a READY layer
  moves it; repeating that signature a frame later does not.
- `Alpha change serial moves when a layer is evicted, released or dropped`: an entry aging out in `begin_update`,
  an eviction at the cap, a layer released for a freed texture, a `configure` with a new layer size and `release` each
  move it; `release` on a store that is no longer configured, and a later `begin_update`, do not.
- `Alpha store streams cells out and in 200 times`, run with textures freed and kept alive: 18 textures in three cells
  (6 textures with 2 cutoffs and 2 offsets, then 4 and 8 with one each) over 16 layers, on a 3-phase cycle (first and
  second cells, second and third, third alone) of 3 frames each, 1800 frames. In the freed run every odd cycle frees
  the even textures of a cell as it leaves, and they return with fresh validators in the same slots; the odd textures
  keep their RIDs. Every frame: every acquired index names the asked texture, cutoff, scale, offset and the layer's
  generation; no layer's texture is freed after `begin_update`; every entry that is not a tombstone matches its
  layer's generation, and a READY one also its fill's; no refusal when this frame's and the last frame's pairs fit in
  16; at most 16 slots; at most 37 entries (36 entries over two frames plus the null entry). Both runs evict; only the
  freed run releases.
- `Alpha accept rule known answers`: a CPU copy of `ray_scene_alpha_passes` and `ray_scene_hash` (the coverage lookup
  is a stand-in with REPEAT on x and no bilinear filter). Index 0 and a PENDING entry pass; the UV is
  `uv0 * w0 + uv1 * u + uv2 * v`, then scale and offset; threshold mode passes at exactly .5 and not just below;
  stochastic mode always passes at coverage 1, never at 0, and passes within .01 of 30 % at .3 over 65536 seeds.
- `Hardware barycentrics swapped give the software hit coordinates`: software (u, v) from a CPU Moller-Trumbore on
  (v0, e1, e2); hardware (b1, b2) as Vulkan defines them for the soup order v0, v0 + e2, v0 + e1. `b.yx` equals
  (u, v) and gives the vertex-interpolated UV; `b.xy` does not.
- `Alpha fill keeps a grate's open share in stochastic mode`: a 1024 squared binary grate (period 10, bars 4, open
  share .357) box-averaged to 256 squared as `ray_scene_alpha_fill.glsl:main` does for a source at least the layer
  size. The stochastic expected open share, with the hash's exact pass chance `ceil(c * 2^24) / 2^24`, equals the
  mip 0 share within 1e-6, and one hashed ray per texel lands within .01. Rounding each texel to k / 255, as the R8
  store does, moves the mean by at most .5 / 255. Threshold mode at .5 leaves .159 open, more than .1 from the mip 0
  share. The fill's small-source branch is not modelled.
- `Alpha store keeps Vault111Cryo's grate footprint open: 256 box fill and linear sample, against a 512 nearest cast`:
  a known-answer test on game data that is not in the repo. It reads the directory named by
  `GARDEN_RAY_SCENE_GRATE_DIR`. When that is unset, it prints a line starting `SKIP:` and passes without checking
  anything. The directory holds `grate_points.tsv` (5,500 rows of kind `grate`: 22 fixtures by 250 points, with the
  hit triangle's corner UVs, barycentrics, the material's threshold, opacity, texture range, UV scale and offset, and
  the nearest mip 0 alpha a reference cast read) and one `<texture>.alpha0.512x512.r8` plane of 512 by 512 bytes per
  texture.
  - The test builds each layer as the store would, one per (texture, cutoff): the cutoff as
    `RayScene::_alpha_entry` computes it (a missing threshold reads as .5, a missing opacity as 1), and `fill_texel`
    taking the share of each 2 by 2 source block at or above the cutoff, rounded to k / 255. It requires a non-zero
    texture range and a cutoff above 0 on every row. It packs the corner UVs as half floats, interpolates and applies
    scale and offset as `ray_scene_inc.glsl` does, samples bilinear with REPEAT like the store's sampler, and counts a
    point as open with chance one minus the stochastic pass chance.
  - Checks: the test recomputes the cast's nearest texel from the corner UVs in double and finds every one equal to
    the stored alpha; the cast mean (open where nearest alpha times opacity is below the threshold) is .3438 within
    .0005; the engine mean lies between .308 and .368, within .01 of the cast mean, and within .001 of .3469. It
    prints each fixture's engine, threshold-mode and cast shares.
  - Limits: when the grate's alpha holds only 0 and 255, the cut-off edge is the only place the box fill and the
    bilinear sample can move a point, and every cutoff from 1 to 255 gives the same layer, so the cutoff formula is
    mirrored but not exercised. Threshold mode is printed, not checked, so the test does not tell the two modes apart.
  - It is not a GPU test: the half-float pack, interpolation and sampling are CPU copies.
- `Alpha entry key equality, canonicalisation and invalid far values`: exercises `RayAlphaStore::canonical_key` and
  `acquire` on clamps, upper-bit clearing, cull 3 and mode 3 mapping, mode 0 zeroing, `-0.0f` to `+0.0f` normalisation,
  non-finite far fallback to mode 0 with per-frame `stats.far_invalid` reset and 100-frame NaN no-churn, single-field
  key distinctness and the 4-argument `acquire` overload.
- `Alpha table entry layout and mode 1 byte offsets`: `static_assert`s on `EntryKey` (40 B) and every `TableEntry`
  (56 B) field offset, entry 0 all zeros, and a mode 1 entry checked byte-by-byte with finite `+0.0f` `density` at
  offset 48 and `LAYER_EMPTY` `far_state` at offset 52.
- `Ray facing helper and alpha cull truth table`: exercises `RayScene::ray_backface` on both quad triangles and the
  full 24-row truth table for `RayScene::alpha_culled` (6 culled: 4 non-light, 2 shadow non-two-sided, 0 shadow
  two-sided), plus invariance under far-mode bits and non-bit-5 material flags.
- `Binding 9 triangle UV record size and zero fourth word`: `static_assert`s on `RayScene::TriangleUv` (16 B,
  trivially copyable, not trivially default-constructible) and `LocalVector::resize` zero-fill of `uv[0..3]` across
  reuse.
- `One key change allocates one entry and retires the old one two frames later`: covers `cull_back` to `cull_disabled`,
  probe `k` step and compile one-shot (`cull_disabled` to `cull_back`), checking the `last_used + 1 < frame` retire
  timing, zeroed tombstone and LIFO `free_entries` reuse.
- `Cut-out quad culls both triangles by cull mode and light-ray input`: CPU mirror trace (`moller_trumbore`,
  `alpha_passes`, `trace_instance`) across both quad triangles and both alpha modes for `cull_back` and `cull_front`
  under GI `(false, false)`, non-light any-hit `(true, false)` and shadow `(true, true)`.
- `Mirrored cut-out instances cull in object space`: object-space facing on `M1` z-mirror and `M2` x-mirror across
  GI, non-light any-hit and shadow rays.
- `Opaque, pending and cull-disabled quads are unchanged`: opaque, pending, empty and `cull_disabled` quads stop all
  ray kinds from both sides, plus opaque `M1` `backface` reporting (`+z` true, `-z` false).
- `Shadow rays and DOUBLE_SIDED on the cut-out quad`: builds slot flags via `RayScene::shadow_cast_flags` and
  `RayScene::slot_cast_flags` and checks plain and `M1` cut-out quads for cast `ON` versus `DOUBLE_SIDED` with the
  light at `+z` and `-z`, plus GI and non-light any-hit rays unchanged under `DOUBLE_SIDED`.
- `Alpha far trigger follows mode 1 or 2 use and refills`: drives `layer_needs_far`, `commit_fill`, `commit_far`,
  `begin_update` and `acquire` through mode 0 ready, late mode 1 on a ready layer, mismatched and matching
  `commit_far`, refills, found-entry re-acquire, mode-0-only clearing, mode 2 on an empty layer, released-layer reset
  and NaN-downgraded mode 0.
- `Alpha far state publishes on the next begin_update`: checks `TableEntry::far_state` stays `LAYER_EMPTY` in the
  frame of `commit_fill`, `commit_far` and `sync_entries`, publishes `LAYER_READY` with `get_change_serial()` moved on
  the next `begin_update` (both same-frame fill and settled-layer `commit_far`), immediate `LAYER_READY` for a new
  mode 2 entry after commit, refill drop on the next `begin_update`, and mode 0 keeping `far_state == 0` throughout.
- `test_ray_scene_cast.cpp`: `Cast setting flag known answers` (`RayScene::shadow_cast_flags`), `Shadow and bounce rays
  per cast setting` and `The double sided caster bit goes only onto live cut-out slots` (`RayScene::slot_cast_flags`
  and `RayScene::CANDIDATE_FLAGS`).
- `test_ray_scene_class.cpp`: `always_accepted` treats any alpha index as not accepted outright. The flag sweep in
  `A surface is opaque to hardware only when every software ray accepts it outright` adds alpha indices 1, 5 and
  0xFFFFFF (bits 8 and up) to the 64 low flag combinations (bits 0-5), both occluder values and every cast setting; the
  split test has two cut-out rows. Known answers: `surface_candidates(s, 1 << 8)` sets bit `s`, and a pending surface
  (index stripped) is not a candidate, so its instance is OPAQUE.
- Not covered: the device paths (`RayAlphaStore::update`, the fill shader, the table upload, the serial moves inside
  `update`), `RayScene::update` with alpha on, and the `FLAG_RAY_SCENE_EXCLUDE` admit test in
  `renderer_scene_cull.cpp:RendererSceneCull::_scene_cull`.
