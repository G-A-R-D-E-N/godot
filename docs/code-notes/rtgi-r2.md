# R2: ray-traced diffuse as the compositor slot's first provider

### `servers/rendering/renderer_rd/environment/rtgi.cpp`

Default off. Rays follow the ray scene's backend, software or hardware. With `rtgi/enabled` false no shader, pipeline or
status file of this pass exists, and the DDGI trace variant stays uncompiled, because `TRACE_MODE_RTGI` (`MODE_RTGI` in
GLSL) sits in its own `ShaderRD` group.

`RtgiPass` is a static singleton made on the first enabled frame, like `GICompositor`, so no renderer member or init
hook exists. `GICompositor::finalize` frees it first. The destructor is public because `memdelete` runs it from outside
the class. The constructor stays private.

Frame order, before `GI::process_gi`: classify tiles, `DDGI::trace_rtgi`, temporal, variance, a-trous, compose into the
slot's radiance and coverage, publish the record. The slot blends them in `gi_extra_compose.glsl` after the GI dispatch.

`RtgiPass::_process` refuses a frame, with the reason in the status and one printed line per change, on: the synthetic
provider on, multiview, a DDGI volume that isn't ready, a ray scene that isn't ready, no depth or normal, or a trace that
didn't dispatch. The last matters because `candidate_second` holds last frame's flags when no
trace ran, and they'd read as this frame's rays. `buffers->refused` clears at the end of a frame that completes, so a
refusal prints only when it changes.

`RenderForwardClustered::_pre_opaque_render` tests `RB_SCOPE_DDGI` in the pass's gate, and its `else` calls
`RtgiPass::release`. So R2's textures go when DDGI goes, with SDFGI or VoxelGI on too, and with no DDGI no pass is built and
no status file is written.

While the ambient buffer doesn't exist, `GICompositor::ensure_slot_textures` returns `Size2i()` and that frame is skipped
without a refusal. It's the one-frame case on a new viewport or a size change.

A change of the world or sky epoch sets `FRAME_FLAG_SOFT_CUT`, and the temporal pass clamps the history count to
`soft_cut_count`, 0 by default. So such a bump restarts history from one sample. The scene epoch does not cut: it moves
on every change of the ray scene, and local changes are the restart spheres' business. See `gi_epochs.h` below.

### `servers/rendering/renderer_rd/environment/rtgi.h`

`FrameInputs.hardware` is the ray scene's backend for the frame, so a switch between software and hardware rays is a
`CUT_BACKEND` history cut. `DDGI::trace_rtgi` takes the hardware trace shader when the ray scene uses hardware rays,
and refuses hardware for the ray scene if that pipeline cannot be made.

### `servers/rendering/renderer_rd/shaders/environment/rtgi.glsl`

Counters, under `RTGI_COUNTERS` on binding 15, chosen once at lazy start from the internal `rtgi/gpu_counters`, so
default variants have no atomics. `RtgiPass` reads them back with a blocking `buffer_get_data` while it's on, so a timing
run keeps it off.

- Classify counts tiles total, tiles traced and trace pixels.
- Temporal counts from `candidate_second`: rays (`NO_CANDIDATE` clear), hits (`NO_CANDIDATE` clear and `MISS_SKY` clear),
  sky misses (`NO_CANDIDATE` clear and `MISS_SKY` set), back faces (`BACKFACE`). A skipped pixel has `MISS_SKY` clear, so
  hits and sky misses need both tests.
- Those four are counted before the early return for non-finite radiance, because counting after it would hide the
  pixels the pass throws away. History rejects stay after the returns.

### `servers/rendering/renderer_rd/environment/gi_epochs.h`

A word of 0 means unknown. `next` wraps past 0 to 1, so a word that has moved never reads 0 again. `fill_record` sets a
flag only for a nonzero word.

`GIEpochs` and `RayScene` are singletons, the tracker is `RayScene`'s member, and they assume one ray-scene viewport. Two
3D viewports that update the ray scene in turn move the origin epoch and rebuild its buffers on every update when their
cameras sit in different anchor cells. They move the scene epoch when the instances they cull differ, and the world
epoch when those sets share no instance. Two viewports in one cell that cull the same instances move nothing.

The producers:

| Source | Bumped by | When |
|---|---|---|
| `SOURCE_SCENE` | `RayScene::update` | once per update whose ray-scene content changed: an instance added, removed or moved, its mesh built again, or its layer mask or material entries changed; once at the first update; never on a later update with no change |
| `SOURCE_WORLD` | `RayScene::update` | when no instance of the last non-empty scene is left: the world was replaced or the scene emptied |
| `SOURCE_SKY` | `SkyRD::sky_set_material`, `RendererSceneRenderRD::render_scene` | a sky's material replaced, or a viewport drawn with a different sky than on its last frame |
| `SOURCE_ORIGIN` | `RayScene::update` | the ray scene's anchor snapped to a new cell, and once at the first update |
| `SOURCE_LIGHT` | `RtgiPass` | a lamp change, a directional jump or a repeated directional read from the light tracker, and once at first use |

`GISceneTracker` decides the scene and world bumps, and holds no GPU state, so the doctests cover it. Its signature is a
sum over the instances of one 32-bit hash each, with 2^32 added per instance, so it does not depend on the order the
cull hands the instances in, and adding an instance changes it. A changed instance goes unseen with odds of about 2^-32.

A member is the geometry instance's address mixed with its mesh RID. Geometry instances come from a paged allocator that
hands freed slots out again, so the address alone would read a new world's instances as kept. The set is rebuilt only
when the members' own order-free sum changes, also on an update whose scene did not change, so a moved instance costs no
rebuild and a later change compares with the current set. The world rule reads only membership, so a moved instance is a
scene change and not a world change. An instance that stays across a world swap, a held item for one, keeps the world
epoch from moving. The world epoch moves when the scene empties and not again when it refills, and an update in which
every mesh is skipped counts as emptied.

### `servers/rendering/renderer_rd/environment/ray_scene.cpp`

`RayScene::update` hashes per instance: the mesh RID, the mesh's build serial, the world transform (not the
anchor-relative one, so an anchor snap is not a scene change), the layer mask and the instance's material entries.
`RayScene::_build_blas` gives every build a new serial from a counter in `RayScene`. The counter outlives the entry that
`mesh_cleared` erases, so a mesh captured again under the same RID is a change, also with the same triangle count and
bounds. A build follows a surface added to the mesh, or the mesh cleared and captured again, so a mesh rewritten every
frame with the same content moves the scene epoch every update, along with the rebuild it causes.

`RayScene::update` bumps the scene epoch right after its instance loop, before anything later in the update, so a
consumer that stamps a frame later in the same update stamps it with the new value.

The plain ray-scene hash (`instance_signature`) can't drive a cut: it is anchor-relative and `material_offset` is a
running count, so it changes on an anchor snap and with the cull order. It only decides the rebuild.

The scene epoch moves on every update while anything in the ray-scene region moves, or while a game animates an emission
scale that goes into the material entries. R2 does not read it.

### `servers/rendering/renderer_rd/environment/sky.cpp`

`SkyRD::sky_set_material` bumps the sky epoch only when `Sky::set_material` reports a new material. A sky changing
gradually through its shader parameters is not a replacement and bumps nothing; the temporal pass follows it.

### `servers/rendering/renderer_rd/renderer_scene_render_rd.cpp`

`RendererSceneRenderRD::render_scene` hands the frame's sky RID, none without an environment, to
`RenderSceneBuffersRD::update_gi_sky`. That keeps the RID per viewport and reports a change from the viewport's last
frame. So a sky swapped inside one environment bumps, and so does a switch to an environment with another sky. A switch
between environments that share a sky does not, and neither does a viewport's first frame, also after its buffers
object was made again. Reflection probe passes are skipped. A background mode changed with the sky RID kept bumps
nothing.

### `servers/rendering/renderer_rd/environment/gi_compositor.cpp`

The early-out also tests `RtgiPass::is_enabled`, so R2 on with `compositor/extra_provider` off keeps the textures R2 just
wrote, and the state reads `GI_COMPOSITOR_SLOT_DISABLED`. The slot-on and slot-off pair compares that way.

`ensure_slot_textures` makes the `ViewportState` first, because R2 runs before `_process` and the early-out erases it
while R2 is off. `publish` and `get_record` return quietly with no state, so `get_custom_data`'s error path never prints.

The AO relief for the slot's light isn't here. The slot's light takes full SSAO and multi-bounce AO.

### `servers/rendering/renderer_rd/environment/ddgi.cpp`

`TRACE_MODE_RTGI` is in `TRACE_GROUP_RTGI`, disabled at boot, because validity is all or nothing within a `ShaderRD`
group: in group 0 a failed R2 compile would take the probe, debug-ray and surfel traces with it, with R2 off.
`DDGI::init` skips its pipeline, since the disabled group's shader is a placeholder. `DDGI::trace_rtgi` enables the
group and makes the pipeline on first use.

With R2 on, a failed R2 compile crashes after printing the glslang error instead of refusing, because `valid` stays true over the
emptied `variants`. R2-off runs don't compile the block, so a change elsewhere can break it unseen: compile the `MODE_RTGI`
variants at every engine re-sync. The shader baker skips a disabled group.

### `servers/rendering/rendering_server.cpp`

| Internal setting | Default | Meaning |
|---|---|---|
| `rtgi/gpu_counters` | false | counters on, read at lazy start only |
| `rtgi/debug_status_path` | "" | JSON written by `RtgiPass::_write_status`, rewritten each frame while set |
| `rtgi/debug_view` | 0 | 1 tile class, 2 ray validity, 3 contribution, 4 history count |
| `rtgi/debug_dump_path` | "" | the debug view as raw RGBA16F at trace size, plus `<path>.txt`, `width height view` |

In the validity view a rejected pixel is raw (1.0, 0.5, 0.0). In the contribution view red is coverage, raw linear.

No public status API and no debug-draw enum exist, so nothing draws these views in the editor.

### `tests/servers/rendering/test_gi_epochs.cpp`

Covers the epoch rules: the record flags, the wrap past 0, the world word in the snapshot, the order-free scene
signature, no bump on an unchanged update, one step per change, the world rule (replaced, cleared, refilled from empty,
an instance moved), the member set refreshed on an unchanged scene, and a reused address with another mesh. That the
bump comes before later readers in the same update follows from its place in `RayScene::update` and has no test.

Two things can't be tested without a device, because `RenderSceneBuffersRD`'s destructor frees samplers through
`MaterialStorage`: a publish after the early-out erased the state, and the per-viewport sky of `update_gi_sky`.
