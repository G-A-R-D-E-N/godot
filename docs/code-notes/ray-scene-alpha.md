# Ray scene alpha cut-outs

`servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene` can trace alpha-tested (cut-out) surfaces as
cut-outs: rays pass through the holes of foliage, grates and fences instead of stopping on the whole triangle. It is
off by default. With `rendering/global_illumination/ray_scene/alpha_test` false, the ray scene uploads, shader defines
and bindings are the same as without this code.

The DDGI trace shader reads cut-outs in all four of its modes (probe, surfel, debug rays and the per-pixel ray-traced
GI trace `TRACE_MODE_RTGI`), and in the shadow rays those modes trace. The ReSTIR GI module's ray shader
(`modules/gi_restir/shaders/restir_gi.glsl`) reads them too, in every ray variant, so the rays that re-trace and
re-shade the `TRACE_MODE_RTGI` samples test the same cut-outs. In Stochastic mode they draw the same random value as
the ray that made the sample only where they pass its seed (see "Bindings and defines"). The ray-traced direct shadow
pass (`direct_shadows.cpp`) builds its trace shaders without the alpha define, so it does not apply cut-outs. A
cut-out surface casts a solid shadow there, while DDGI and ReSTIR GI rays pass through its holes.

## Settings (`servers/rendering/rendering_server.cpp`)

- `RenderingServer::init` defines five settings in the ray scene group, next to `backend` and
  `skip_additive_surfaces`. `doc/classes/ProjectSettings.xml` documents them.

| Setting (`rendering/global_illumination/...`) | Default | Read |
|---|---|---|
| `ray_scene/alpha_test` | false | `RayScene` constructor (capture, store, bindings, defines): restart |
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
  `layer_size` squared, allocated in full on the first update: 32 MiB at the defaults, 128 MiB at 512 squared.
- `RayAlphaStore::acquire`: the layer key `LayerKey` is the whole 64-bit texture RID and the cutoff, 1 to 255. A
  cutoff of 0 or above 255, a null RID, and a scale or offset that is not finite return index 0 (no layer). It
  returns the table entry index, or 0 when the texture is already freed or no layer can be had. Entries are deduped
  on (layer, scale, offset) and the layer's generation. A hash collision appends a second entry, which is never found
  again and ages out with the rest. Calling it every frame for every material (the material cache is rebuilt every
  frame) adds nothing after the first time and stamps the layer and the entry with the frame.
- `RayAlphaStore::TableEntry` (24 B, binding 13 in the trace shaders): layer, state, UV scale, UV offset. Entry 0 is
  the null entry.
- `RayAlphaStore::update`: needs a RenderingDevice. It marks a layer PENDING when its source RD texture is missing,
  when its signature (RD texture id and size) differs from the filled one, or when it is not READY. It fills pending
  layers on the GPU within the per-frame texel budget (always at least one), marks each READY through `commit_fill`,
  copies layer states into the table entries through `sync_entries` (entry 1 up, so the null entry never takes layer
  0's state) and uploads the table when anything changed. Free layer slots are skipped. `commit_fill` and
  `sync_entries` need no device, which the unit tests use.
- `ray_scene_alpha_fill.glsl:main`: when the source is at least `layer_size` on both axes, each layer texel is the
  share of mip 0 source texels in its footprint whose `round(alpha * 255) >= cutoff`. Otherwise (either axis smaller)
  it is one sample of the source with the LINEAR, REPEAT sampler at the layer texel centre, set to 1 when
  `alpha * 255 >= cutoff` and to 0 otherwise.
- A layer whose source RD texture goes missing turns PENDING. When the same texture comes back, its signature equals
  the filled one, but the layer is not READY, so it is refilled.
- `RayAlphaStore::fill_shader` is a pointer, made in `_ensure_pipeline` and deleted in `release`. The `ShaderRD`
  constructor asks the RenderingDevice for its vendor name, so a member shader would need a device to build the
  store. With the pointer, `configure` and `acquire` run with no device, which the unit tests use. Each pipeline gets
  a fresh shader object, because `ShaderRD::initialize` refuses a second call on the same object.

## Store: layer and entry lifetime

- Textures send no free signal (`TextureStorage::texture_free` tells only the decal and area-light atlases), so the
  store polls. `RayAlphaStore::set_liveness` takes a function. `RayScene` passes `_alpha_texture_live`, which is
  `TextureStorage::owns_texture`: false for a freed slot and for a slot reused under a new validator. With no function
  set (the device-free tests) every texture counts as live.
- `RayAlphaStore::begin_update(frame)` runs in `RayScene::update` right after `configure`, before the instance loop,
  with `RSG::rasterizer->get_frame_number()`. It resets `layers_refused` and `dead_sources_refused`, releases every
  layer whose texture fails the liveness test, and retires every entry whose layer was released or whose last use is
  more than one frame back.
- `RayAlphaStore::_release_layer`: erases the layer's key, clears the slot (EMPTY, no source, sizes and signatures 0),
  increments its `generation` and puts it on `free_layers`. A reused slot is EMPTY, so the refill test queues it and
  the new entry copies EMPTY: nothing samples the old texels before the full-slice fill overwrites them.
- `RayAlphaStore::_retire_entry`: erases the entry's key only when it still maps to that entry, writes the tombstone
  (`layer = ENTRY_RETIRED`, EMPTY, zero scale and offset) and puts the index on `free_entries`. Entries never shrink
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
  and total fill and fill texel counts, and the totals `layers_released_total`, `layers_evicted_total` and
  `entries_retired_total`. `RayScene::_print_alpha` adds the released and evicted totals to its line.
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
  final material records, alpha index bits included, into `GISceneTracker` after `_alpha_instance` has run. A surface
  that starts or stops being traced as a cut-out moves the scene epoch on the update where its record changes, and so
  does a new entry index for the same cut-out. A layer refilled with new contents while it stays READY, and any table
  or layer change that leaves every material record as it was, move the serial and no epoch. A READY layer that turns
  PENDING is solid in that frame's trace through the shader's own READY test, and the scene epoch follows one update
  later, when `_alpha_instance` clears the index. The epochs do not read the serial.
- Cost of that relation: every instance of a cut-out surface changes its record when its layer turns READY, is
  evicted or is acquired again. A fill or churn burst (a level load, or more alpha pairs in view than
  `max_alpha_layers`) therefore bumps the scene epoch and publishes a dirty box per instance. Past `dirty_box_cap`
  merged boxes the feed overflows, and ReSTIR GI cuts its history for the frame (`restir_epochs.cpp`,
  `RESTIR_CUT_SCENE_OVERFLOW`) instead of a local reset. Not measured yet.
- The table buffer and the layer texture RIDs can change too (the table grows, the store is reconfigured). The serial
  is not a signal for that: bind them again every frame through `RayScene::append_uniforms`.

## Texture coordinates (`servers/rendering/renderer_rd/environment/ray_scene.cpp`)

- `RayScene::RayScene`: reads `ray_scene/alpha_test` once into `alpha_enabled`. Everything below runs only while it is
  true. With it false nothing is captured, no buffer is made and bindings 9, 13 and 14 are never pushed.
- `RayScene::_capture_surface`: next to the positions, the surface's `ARRAY_TEX_UV` is packed per vertex as one half2
  word (`RayScene::pack_half2`, round to nearest even) into `MeshEntry::uvs`, parallel to `MeshEntry::positions` (a
  vertex without UVs gets 0). `MeshEntry::surface_uv_extent[surface]` keeps the largest absolute UV component in
  floats, for the half-float guard (`_surface_uv_unsafe`). The record is captured for every mesh, opaque ones
  included, because meshes are shared across instances with different materials and materials change at runtime.
- `RayScene::_build_blas`: `MeshEntry::bvh_uvs` holds one `TriangleUv` (three half2 UVs, 12 B) per triangle in the
  same BVH order as `bvh_triangles`. `uv[k]` belongs to the triangle's index `t * 3 + k`, so `uv[1]` sits at
  `v0 + e1` and `uv[2]` at `v0 + e2`.
- `RayScene::_assemble`: appends every mesh's `bvh_uvs` in the order of the `triangles` push, so the record of global
  triangle `i` is entry `i` (binding 9, read with the triangle index both backends already return). The assembly
  vector is local and freed after the upload, so the only kept CPU copy is the per-mesh one. Like the triangle buffer,
  it is uploaded whole on every assembly, and gets one zero record when the scene has no triangles, so the buffer is
  never null after the first assembly while alpha is on.
- `RayScene::get_triangle_uv_buffer`: the buffer ("Ray Scene Triangle UVs"), freed in the destructor.
- Cost: 12 B per triangle on the GPU and once on the CPU, plus 4 B per captured vertex. A scene of 1,427,848
  triangles takes 16.3 MiB on the GPU.

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
- `RayScene::alpha_uv_unsafe(extent, scale, layer_size)` holds that math as a static that needs no device.
  `_surface_uv_unsafe` keeps its own past-the-range test before it reads the table entry, then calls it.
  `RayScene::float_to_half` and `RayScene::pack_half2` are public statics for the same reason.
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
  used, table entries, and the layers released and evicted so far. The store's own `layers_refused` resets in
  `begin_update`; refusals are counted here, per material, as `store full`. That count also takes a material whose
  texture is already freed and one with a scale or offset that is not finite, since `acquire` returns 0 for both; the
  store's `dead_sources_refused` holds the first on its own. It does not wait for a TLAS rebuild: a software READY
  change touches no instance record.
- `RayAlphaStore::get_entry_count` bounds the index in `_alpha_live`.

## Software accept rule (`servers/rendering/renderer_rd/shaders/environment/ray_scene_inc.glsl`)

- Under `RAY_SCENE_ALPHA` the include declares binding 9 (`ray_scene_triangle_uvs`, three packed half2 words per global
  triangle), binding 13 (`ray_scene_alpha_table`, the store's 24 B entries) and binding 14 (`ray_scene_alpha_layers`,
  the layer array).
- `ray_scene_alpha_passes`: index = flags >> 8. Index 0 and a table entry that is not READY pass (solid). Otherwise the
  UV is `uv0 * (1 - u - v) + uv1 * u + uv2 * v`, then `* scale + offset`, and the coverage is `textureLod` at level 0
  (the array has one mip). Threshold mode (`RAY_SCENE_ALPHA_THRESHOLD`) passes at coverage >= 0.5. Stochastic mode
  passes when coverage > `(hash(hash(seed ^ triangle * 0x9e3779b9) + instance) >> 8) / 2^24`.
- Seed: the caller's per-ray seed after `RAY_SCENE_COVERAGE_SEED`, the same value the BLEND coverage test gets, mixed
  with the triangle and the instance, so two instances of one mesh draw separately. The alpha hash differs from the
  BLEND hash, so a surface that is both BLEND and cut-out draws two streams.
- `ray_scene_trace_blas`: the test sits after the skip mask and before the band test and the tie limit, before
  anything writes `r_hit`. A rejected cut-out never becomes the hit and never changes `r_hit.blend`, so it cannot move
  the tie band either.

## Bindings and defines (`servers/rendering/renderer_rd/environment/ray_scene_backend.cpp`)

- `RayScene::append_uniforms`: with `alpha_enabled`, pushes bindings 9 (`triangle_uv_buffer`), 13 (the table) and 14
  (the layer array with the default LINEAR, REPEAT_ENABLED sampler) for both backends. Every consumer of the include
  binds its ray scene set through this call.
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
  `append_uniforms`. With `gi/replay_seed`, its replayed rays (FINAL, SOURCE, VALIDATE) pass the stored seed, RTGI's
  seed at that pixel, so on the same triangle and instance index a stochastic cut-out compares its coverage with the
  same random value as the `TRACE_MODE_RTGI` ray that made the sample. Without `gi/replay_seed` (the default), the
  fresh-Lo rays (`rs_fresh_lo`, FINAL and VALIDATE) take a seed from the receiving pixel and frame, and the
  visibility rays (`rs_visible`, in `restir_gi_inc.glsl`) always do, replay or not. On a texel with coverage between
  0 and 1 each of those rays is a new draw. A sample that stopped on a texel of coverage c is found again only with
  probability about c. A failed re-trace sets `hit_here` false, so the sample counts as blocked (weight 0,
  `INFO_VIS_BLOCKED`) and its Lo is not refreshed. Cut-out edges therefore come out darker by about a further factor
  c, not only noisier, and visibility there changes from pixel to pixel and frame to frame. Coverage 0 and 1, and
  Threshold mode, give the same answer every time. The BLEND coverage test already behaves this way. Which fix the
  module takes is open: seeding `rs_visible` and `rs_fresh_lo` from the stored sample, or keeping a sample whose
  re-trace failed on a cut-out hit. Threshold for the module's variants alone does not fix it with `gi/replay_seed`,
  because the `TRACE_MODE_RTGI` ray that makes the sample stays stochastic, so stored-seed re-hits would disagree on
  partly covered texels. The module reads the dirty-box feed (`FEED_MODULE`), whose state word hashes the final
  material flags, alpha index bits included (`RayScene::feed_surface_state`): a surface that starts or stops being a
  cut-out publishes a box, a READY layer refilled with new contents does not.
- `servers/rendering/renderer_rd/environment/direct_shadows.cpp`: its `defines` do not include
  `RayScene::alpha_shader_defines()`. It still binds its ray scene set through `append_uniforms`, so the alpha
  bindings are pushed and its shader does not declare them. To apply cut-outs there, add the call to its defines;
  its history would then also need the change serial.
- `RayScene::is_ready`: with alpha on it also needs the UV buffer, the table and the layer array (`_alpha_ready`). All
  three exist after the first `update`.

## Hardware accept rule (`servers/rendering/renderer_rd/shaders/environment/ray_scene_hw_inc.glsl`)

- `ray_scene_trace_masked`, main candidate loop: `ray_scene_alpha_passes` runs after the tie limit reject and before
  the BLEND coverage miss, so a rejected cut-out is never confirmed. It uses the same table, layer, seed
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

## Unit tests (`tests/servers/rendering/test_ray_scene_alpha.cpp`, `test_ray_scene_class.cpp`)

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
- The lifetime and serial cases below use `test_texture_live`, a liveness function backed by the test's own list of
  live ids, and drive frames with `begin_update`, `acquire`, `commit_fill` and `sync_entries`.
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
  `sync_entries` leave it still, and so does a `configure` that only changes the texel budget.
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
- `test_ray_scene_class.cpp`: `always_accepted` treats any alpha index as not accepted outright. The flag sweep adds
  alpha indices 1, 5 and 0xFFFFFF (bits 8 and up) to the low flag combinations, both occluder values and every cast
  setting; the split test has two cut-out rows. Known answers: `surface_candidates(s, 1 << 8)` sets bit `s`, and a
  pending surface (index stripped) is not a candidate, so its instance is OPAQUE.
- Not covered: the device paths (`RayAlphaStore::update`, the fill shader, the table upload, the serial moves inside
  `update`), `RayScene::update` with alpha on, and the `FLAG_RAY_SCENE_EXCLUDE` admit test in
  `renderer_scene_cull.cpp:RendererSceneCull::_scene_cull`.
