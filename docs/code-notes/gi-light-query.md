# GI head light query: the field luminance at a point, read back per slot

### `servers/rendering/gi_light_query.h`

`servers/rendering/gi_light_query.h:GiLightQuery`. Engine side of the GI light query (design note rev 3, "GI head light
query"). For each query (slot, position P, unit facing F, sun factor s) the pass returns the luminance of a white
Lambertian card at P facing F, L = E(P, F) / pi, in cd/m^2: linear RGB, Y with the Rec. 709 weights, the DDGI coverage,
`body_y` (Y of the directional term alone), the slot, the frame and flags. Nothing runs, no pipeline is compiled and no
buffer exists until a query list is set.

This class is the CPU half, free of any device so the doctests can drive it (design "Readback", "Interface"). It lives in
`servers/rendering`, not `renderer_rd`, because `RenderingServerDefault` reads it directly and `rendering_device` is an
optional part of the build (`servers/rendering/SCsub`); a getter in `renderer_rd` would not link in a build without it.

- `Query`, `QueryHeader` and `Result` are the std430 layouts of the two set-3 buffers. `Result` is 48 bytes, checked by
  `static_assert` and by the layout test. Slots travel as `uint`, never as floats.
- `QueryHeader::sun_mask` is a bit per ray-scene light index (directional lights come first in that list): the lights s
  applies to (see `RayScene::cloud_mapped_mask` below).
- `Flag` holds the result flag bits. `shader_defines()` writes them, and the sample counts, disc radius, point floor and
  shadow bias, as `#define`s into the `MODE_LIGHT_QUERY` variant text, so C++ and GLSL share one set of numbers. Only the
  light-query variant carries them, so the other trace variants' define text is unchanged.
- `ResultField` names the positions in a result row.

### `servers/rendering/gi_light_query.cpp`

`servers/rendering/gi_light_query.cpp:GiLightQuery::validate`, `pack`, `set_requests`, `take_requests`,
`store_results`, `get_results`, `skip`, `finish_frame`, `forget`, `get_singleton`.

- `validate` checks the list before it is packed: 7 floats per query, at most 32, one slot per query, slots 0..31 and
  unique, every value finite, facing not zero. `validation_text` gives the message for the `ERR_FAIL` in the setter.
- `pack` normalizes F and clamps s to [0, 1].
- The table is keyed by viewport RID. Each viewport entry holds its render target (how the render side finds it), the
  current list, and 32 slots with the newest result, its dispatch time and its frame. One mutex guards it; `active`
  counts viewports with a non-empty list, so a frame with no queries tests one atomic and takes no lock.
- `set_requests` replaces the list and bumps a generation. A slot that joins the list records the generation it joined
  at; a slot that leaves it loses its result. An empty list clears every result and stops the dispatch.
- `take_requests` (render thread) finds the entry by render target, records the frame as the newest dispatched frame,
  marks the entry as taken, and hands out the list and its generation.
- `store_results` (render thread, from the readback callback) reads whole 48-byte entries only, skips slots out of
  range, skips a slot that is not requested or joined after the batch was dispatched (generation older than the slot's),
  skips a result dispatched at or before the last skipped frame, and keeps a result only if its frame is not older than
  the stored one (32-bit wrap-safe compares). So a late batch never overwrites a newer one, a dropped batch leaves the
  previous value, a batch dispatched for an earlier owner of a slot is ignored once the slot was left out and asked
  again, and a batch still in flight when the dispatch stopped cannot bring a result back.
- `skip` (render thread) is called by `DDGI::trace_light_queries` when it took the list but could not dispatch (trace not
  usable, light-query pipeline could not be made). `finish_frame` (render thread, end of
  `RendererSceneRenderRD::render_scene`) does the same for every entry of the rendered render target whose list was not
  taken in this frame: DDGI off at run time, GI off for the viewport, no DDGI volume, ray scene not ready. Both drop every
  slot's result and remember the frame, so `get_results` returns no rows for that viewport until a new dispatch lands.
  C# then reads "no result" (`TryGet` false, the fallback), never a frozen row. A viewport that is not rendered at all
  (hidden, update disabled) gets no `finish_frame`; its rows stay and both ages grow, so either is a staleness signal C#
  can test.
- `get_results` copies the requested, valid slots under the mutex into rows `[slot, r, g, b, y, coverage, body_y, frame,
  flags, age_ms, age_frames]`. `age_ms` is the time since the dispatch, from `OS::get_ticks_usec`; `age_frames` is the
  live frame (`RSG::rasterizer->get_frame_number()`, passed in by `RenderingServerDefault::viewport_gi_light_query_results`)
  minus the result's dispatch frame, wrap-safe and never below 0; a landed result is usually one or two frames old.
- `get_singleton` is a function-local static, so no server init or free hook is needed.

### `servers/rendering/rendering_server.h`

`servers/rendering/rendering_server.h:RenderingServer::viewport_gi_light_query`,
`RenderingServer::viewport_gi_light_query_results` (design "Interface"). The setter goes through the normal queued path
(`FUNC3` in `rendering_server_default.h`, forwarded to `RendererViewport`). The getter is declared `const` and is not a
`FUNC*R` macro: `RenderingServerDefault::viewport_gi_light_query_results` in `rendering_server_default.cpp` reads the
mutex-protected table, so it never syncs with the render thread. Both are bound in `rendering_server.cpp` and described in
`doc/classes/RenderingServer.xml`.

### `servers/rendering/renderer_viewport.cpp`

`servers/rendering/renderer_viewport.cpp:RendererViewport::viewport_gi_light_query` validates with `ERR_FAIL_COND_MSG`
(count a multiple of 7, at most 32, then the rest of `GiLightQuery::validate`), packs, and stores the list with the
viewport's `render_target`. `RendererViewport::free` drops the viewport's entry before the render target goes.

Viewport to DDGI: `Viewport::render_target` is made once in `viewport_initialize`; `_configure_3d_render_buffers` makes
`Viewport::render_buffers` with `rb_config.set_render_target(render_target)`; `DDGI::update` keeps the `DDGI::Volume` as
the render buffers' custom data `RB_SCOPE_DDGI`. In `_post_prepass_render` the render side has only the render buffers,
so it keys on `render_buffers->get_render_target()`, which the table maps back to the viewport RID.

### `servers/rendering/renderer_rd/renderer_scene_render_rd.cpp`

`servers/rendering/renderer_rd/renderer_scene_render_rd.cpp:RendererSceneRenderRD::_post_prepass_render` (design "Where
it runs"). After `DDGI::process` (and the surfel update when surfels are on), and only when `GiLightQuery::is_active()`,
calls `DDGI::trace_light_queries` with the render target, the environment, the camera attributes and `FLAG_SURFELS_ON`
when the render buffers hold `RB_SCOPE_SURFELS`.

`servers/rendering/renderer_rd/renderer_scene_render_rd.cpp:RendererSceneRenderRD::render_scene`. After `_render_scene`,
for a viewport render (render buffers, no reflection probe) and only when `GiLightQuery::is_active()`, calls
`GiLightQuery::finish_frame` with the render target and the rasterizer frame number. `_post_prepass_render` runs inside
`_render_scene` only when GI is in use and the buffers hold DDGI or SDFGI, so this is the one place that sees every frame
in which the query was not dispatched.

### `servers/rendering/renderer_rd/environment/ddgi.cpp`

`servers/rendering/renderer_rd/environment/ddgi.cpp:DDGI::trace_light_queries`:

- Returns at once with no volume, no ray scene, a ray scene not ready, or no list for this render target
  (`finish_frame` then drops the results).
- After the list is taken: if the trace is not usable or `_light_query_pipeline_ready` says no, it calls
  `GiLightQuery::skip` for the viewport and returns.
- The two buffers (`Volume::light_query_buffer`, 1056 B; `Volume::light_result_buffer`, 1536 B) are made on the first
  dispatch and freed with the volume in `Volume::free_data`.
- Header: ambient colour and sky share (`_fill_light_query_ambient`), count, the rasterizer frame number (32 bits),
  `sun_mask` from `RayScene::get_cloud_mapped_mask`, flags (`FLAG_TRIMMED` from `RayScene::is_light_list_trimmed`,
  `FLAG_HARDWARE_RAYS`, and the caller's surfels flag).
- Positions move from world to ray-scene anchor space on the CPU in double, as the ray scene stores lights.
- `_trace(TRACE_MODE_LIGHT_QUERY, ...)` binds set 0 with the volume's probe list and ray image as dummies (the variant
  reads neither), sets 1 and 2 as every trace, and set 3 = {queries, results} through the optional set-3 argument; one
  workgroup per query; the push constant is the volume's `trace_constant` from this frame's `process`, so
  `emission_exposure` and the sky values match the probe trace.
- `buffer_get_data_async` on the results, `count x 48` bytes, with `light_query_landed` bound to the viewport, the
  generation and the dispatch time. The callback runs on the render thread when the frame completes and calls
  `GiLightQuery::store_results`.

`servers/rendering/renderer_rd/environment/ddgi.cpp:DDGI::_light_query_pipeline_ready` follows `DDGI::trace_rtgi`
exactly, with the trace shader of the backend in use (`trace_shader` and `trace_shader_version` for software rays,
`trace_shader_hw` and `trace_shader_hw_version` for hardware rays). They are two separate `ShaderRD` objects, so each
gets the group enabled, and its pipeline made, the first time a query runs on that backend:

1. If `TRACE_GROUP_LIGHT_QUERY` is not enabled on that shader: `enable_group` starts the variant's compile on the worker
   threads (or its load from the shader cache), `version_get_shader(version, TRACE_MODE_LIGHT_QUERY)` waits for it, and
   `create_compute_pipeline` is called with the shader when the RID is not null. A null shader makes no pipeline.
2. `get_rid()` on the pipeline (it waits for the build). A null pipeline warns once and returns false, so
   `trace_light_queries` skips the dispatch; the group stays enabled, so nothing is retried. Unlike RTGI it does not refuse
   hardware rays for the whole ray scene, because an optional readout should not change every other pass.

`trace_mode_lazy` makes `init` and `_ensure_trace_hw` skip the variant, as they skip RTGI, so nothing is compiled until a
list is set. The first dispatch on each backend then compiles the variant (or loads it from the cache) and builds its
pipeline on the render thread's frame: a one-time hitch on first use, the same accepted hitch as RTGI's first frame. The
results of that first dispatch land like any other. The hardware trace shader itself is still built synchronously by
`_ensure_trace_hw` the first time hardware rays are used, as for every DDGI pass.

A variant whose compile fails is not handled well by `ShaderRD` itself, and this change does not touch it: in
`ShaderRD::_compile_version_end` (`shader_rd.cpp`, engine dev 773f9349d, lines 765-791) a lazy group whose variant comes
back null frees every enabled variant of the version and empties `variants` while `version->valid` stays true from the
base group, so the `version_get_shader` call that collected the group (and every later one) indexes an empty vector
and aborts; a GLSL failure in a lazy group
leaves its placeholder shader in place and is not seen as a failure at all. The same holds for RTGI's lazy group. It is a
pre-existing engine issue, to be reported and fixed on its own.

`servers/rendering/renderer_rd/environment/ddgi.cpp:DDGI::_fill_light_query_ambient` (design "Where the DDGI term is
sampled") gives the colour the forward pass mixes in where coverage is below 1. It takes the ambient part of
`RenderSceneDataRD::update_ubo` (`render_scene_data_rd.cpp`): background colour times the background energy for a colour
background with ambient source Background; otherwise the ambient colour times ambient energy, and the sky contribution
as the sky share when the ambient comes from the sky. Ambient disabled leaves it 0. Both colors from `render_scene_data_rd.cpp` are then put on the
trace's exposure, not the forward pass's: times the camera exposure normalization whenever camera attributes are valid
(1 otherwise), whatever the light units, the same factor `_fill_sky` writes to `emission_exposure` and applies to the
colour background, and that `RayScene` applies to every light. The shader divides the whole sum by `emission_exposure`,
so all three terms (lights, probes, ambient) must carry it; with the forward pass's rule (exposure only with physical
units, never on a colour background) the ambient would be read as colour / exposure where coverage is below 1.

### `servers/rendering/renderer_rd/environment/ray_scene.cpp`

`servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::_select_lights` sets `light_list_trimmed` when the
frame's directional plus local lights exceed `MAX_LIGHTS` (design "Which lights are in it");
`RayScene::is_light_list_trimmed` reads it.

`servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::_pack_light` sets bit i of `cloud_mapped_mask` for the
directional light packed at index i (directional lights are packed first, so i is its index in the GPU light list) when
that light has a cloud-shadow map bound and points down (above the horizon), the same test the forward pass uses before
it applies the map (`LightStorage::_get_cloud_shadow`). "Bound" is `LightStorage::light_directional_get_cloud_shadow_texture`
valid: that getter already returns a null RID when the texture is null or `cloud_shadow_strength` is 0 or less, so a
light Weather leaves at strength 0 gets no bit, and no separate strength getter is needed. "Above the horizon" is
`direction.y < 0` for the packed `direction = -basis.z`; the forward pass and the sky use `to_light = basis.xform(0, 0, 1)`
with `to_light.y > 0`, the same condition. That is the design's "map bound" case: the light's energy is the clear-sky
value and cloud comes only from the map, so s, Weather's map transmittance at P, belongs to this light and no other. The
moon, fills and any other directional light without a map are left out and keep their packed energy. Only the first 32
lights can carry a bit. The mask is cleared with the light lists each update; `RayScene::get_cloud_mapped_mask` reads it.
A map strength between 0 and 1 is not folded in; s is applied as passed (opencw uses strength 1).

### `servers/rendering/renderer_rd/shaders/environment/ddgi_trace.glsl`

`servers/rendering/renderer_rd/shaders/environment/ddgi_trace.glsl:MODE_LIGHT_QUERY` (design "How the query's direct
term differs", "Where the DDGI term is sampled", "Units"). 64 threads per query (`#elif defined(MODE_LIGHT_QUERY)` for
the local size).

1. All 64 threads sort the ray-scene lights that reach P into three shared lists: directional lights with F.l > 0;
   area lights with P in front and a non-zero range window; point and spot lights with a non-zero value at P. Local
   lights with luminance <= 0 are left out, as `ray_scene_direct_light` leaves them out of its local loop.
2. Threads 1..63 take the samples with stride 63, so there is no cap: 4 per directional light, 8 per area light, 1 per
   point or spot light.
   - Directional: 4 shadow rays from points jittered over a disc of radius `GI_LIGHT_QUERY_SUN_DISC` (0.05 m, so 0.1 m
     across) in the plane across F, one per quarter turn, rotated and pushed out by a hash; reach `ddgi.max_distance`;
     `color x energy x max(0, F.l) / pi / 4` per unshadowed ray, times s only for a light whose bit is set in
     `sun_mask` (`light_query_sun_scale`), times 1 for every other directional light.
   - Point and spot: the `ray_scene_direct_light` formula with the distance in the attenuation floored at 0.1 m; the
     shadow ray runs to the true distance.
   - Area: 8 fixed points, a 2 x 4 grid of cell centres on the rectangle, 4 along the longer side; each is the area branch
     of `ray_scene_direct_light` (`window x area x cos / d^2`) with its own shadow ray, divided by 8.
   - Every shadow ray starts 0.01 m along F (`GI_LIGHT_QUERY_BIAS`) through `ray_scene_unshadowed`. The packed energy is
     used as it is.
   - The seed (disc angle and radius, and the foliage coverage hash inside `ray_scene_unshadowed`) comes from the slot
     and the sample index only, not the frame. Each slot has one fixed pattern in the plane across F, so a still head at
     a shadow or leaf edge reads the same fraction every frame, and the fraction changes only as the head moves; the
     sun term does not flicker by quarters of the sun between results. The price is a fixed per-slot bias of up to one
     ray in four at an edge, which does not average out over frames.
3. Thread 0 meanwhile reads `ddgi_sample_direction(P - camera_to_anchor, F, vec3(0), F, 0, DDGI_NO_VIEW_LIMIT)`: view 0,
   so only the normal bias moves the lookup. The indirect term is `mix(ambient, ddgi.rgb x ddgi.energy, coverage)`, the
   forward pass's own blend (`gi.glsl` `process_gi`, then `mix(ambient_light, buffer_ambient.rgb, buffer_ambient.a)` in
   `scene_forward_clustered.glsl`). The sky share of the ambient is the mean of 8 cosine-weighted samples of the trace's
   own sky (`sky_radiance_in`) around F. A NaN coverage reads 0.
4. `light_query_finite` sets NaN or infinite channels to 0 without clamping: each thread applies it to its `direct`
   and `body` sums before the shared-memory write, and thread 0 to the ambient and the probe term before the mix, so one
   bad sample or texel cannot turn the whole row to NaN (a NaN times a zero mix weight is still NaN). Then a shared-memory
   sum, and thread 0 divides both terms by `params.emission_exposure` (the exposure the ray scene and the probe sky were
   scaled by) and writes the result. `light_query_clean` sets every NaN or infinite channel to 0 and
   clamps the rest to 0 or more, for rgb and for the directional part; the directional part is then clamped to at most
   rgb, so `body_y` never exceeds `y`. Negative directional lights (kept, as `ray_scene_direct_light` keeps them) can
   therefore lower the sum but never make it negative. An exposure of 0, NaN or infinity gives coverage 0 and so
   `FLAG_NO_COVERAGE` with rgb 0: the row says it holds no reading. `body_y` is the Y of the directional sum alone,
   after s. `FLAG_PARTIAL_CLOUD` is set when s < 1 and some directional light carries a `sun_mask` bit,
   `FLAG_NO_COVERAGE` when coverage is 0.

### `tests/servers/rendering/test_gi_light_query.cpp`

`tests/servers/rendering/test_gi_light_query.cpp:[GiLightQuery]`, device-free: the 48-byte result layout and the query
and header offsets (with `sun_mask`); validation and packing; requests found by render target and stopped by an empty
list or `forget`; newest-wins per slot with a late batch and a dropped one, ages in ms and frames; the frame age growing
with the live frame while nothing is dispatched and across the 32-bit wrap; 32 slots landed in shuffled order; a
slot left out and asked again ignoring an older batch; partial and foreign readback data; the 32-bit frame wrap; a frame without a dispatch (`finish_frame`, `skip`) dropping the results and the batches still in flight;
and the flag values the shader gets.

### `servers/rendering/renderer_rd/environment/ddgi.h`

`servers/rendering/renderer_rd/environment/ddgi.h:DDGI::TraceMode` gains `TRACE_MODE_LIGHT_QUERY` and `DDGI::TraceGroup`
gains `TRACE_GROUP_LIGHT_QUERY`, its own default-off group so the variant compiles only on first use (design "Where it
runs"). `DDGI::Volume` gains the two lazily made buffers.

Deviations from the design note, each with the code it is in:

- `servers/rendering/gi_light_query.h:GiLightQuery` is in `servers/rendering`, not `renderer_rd` (see above).
- `servers/rendering/renderer_rd/shaders/environment/ddgi_trace.glsl:MODE_LIGHT_QUERY`: the indirect term is
  `mix(ambient, ddgi x energy, a)`, not `ddgi x energy + (1 - a) x ambient`. `ddgi_sample_direction` divides its sum by
  the coverage, so its rgb is already the covered mean, and the forward pass mixes. The note's own words ("the query
  does the same") match the mix.
- `ray_scene.cpp:RayScene::_pack_light`, `ddgi_trace.glsl:MODE_LIGHT_QUERY`: the engine has no sun or moon role on
  `RSE::LIGHT_DIRECTIONAL`, so "the sun" is read as "the directional light with a cloud-shadow map bound and above the
  horizon", which is exactly the case in which the design has Eye pass the map's s. s scales only those lights; the moon
  and other directional lights are never scaled. `body_y` is still every directional light (sun and moon, as the design
  asks). If Eye passes s < 1 while no map is bound, the engine ignores it, which is the design's "GiLightQuery uses 1".
- `ddgi.cpp:DDGI::_fill_sky`: in engine dev 773f9349d `emission_exposure` is the camera exposure normalization alone;
  `ddgi_diag_gain` is not in this tree. The shader divides by `params.emission_exposure`, so it follows a gain if one
  lands there.
- `ddgi.cpp:DDGI::_fill_light_query_ambient`: the ambient colour and sky share ride in the Queries buffer header, not
  the push constant, which all trace modes share. The sky share comes from the probe trace's sky (LOD 2, DDGI sky
  energy), not the forward pass's radiance octmap at the roughest LOD with the IBL normalization. The ambient colour is
  on the trace's exposure, not the forward pass's (see that section).
- `gi_light_query.h:GiLightQuery::SUN_DISC_RADIUS`: "0.1 m disc" is read as 0.1 m across (radius 0.05 m).
- `ddgi_trace.glsl:MODE_LIGHT_QUERY`: the disc pattern is fixed per slot, not jittered per frame (see step 2).
- `gi_light_query.cpp:GiLightQuery::get_results`: rows carry two more fields, `age_ms` and `age_frames`;
  `gi_light_query.h:GiLightQuery::Flag` has two more flags, `FLAG_NO_COVERAGE` and `FLAG_HARDWARE_RAYS`.
- `gi_light_query.cpp:GiLightQuery::set_requests`: a slot that leaves the list loses its result, and an empty list clears
  all results. To give a slot to another NPC, leave it out of one call first (or use a free slot); otherwise the table
  cannot tell the owners apart.
- `gi_light_query.cpp:GiLightQuery::finish_frame`, `skip`: a frame in which a rendered viewport's list is not dispatched
  drops its results, so the getter returns nothing rather than the last reading.
- `ddgi.cpp:DDGI::trace_light_queries`: the frame is the rasterizer frame number, truncated to 32 bits, not the DDGI
  volume frame; a light-query pipeline failure on hardware rays does not refuse hardware for the ray scene.
- `ddgi.cpp:DDGI::_light_query_pipeline_ready`: the variant and its pipeline are built on the render thread at the first
  dispatch on each backend (RTGI's pattern), so the first list costs a one-time hitch; no engine-wide `ShaderRD` or
  `PipelineDeferredRD` change is made for it.
