# R2: ray-traced diffuse as the compositor slot's first provider

### `servers/rendering/renderer_rd/environment/rtgi.cpp`

Default off, software rays. With `rtgi/enabled` false no shader, pipeline or status file of this pass exists, and the DDGI
trace variant stays uncompiled, because `MODE_RTGI` sits in its own `ShaderRD` group.

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

Epoch change sets `FRAME_FLAG_SOFT_CUT`, and the temporal pass clamps the history count to `soft_cut_count`, 0 by default.
So an epoch bump restarts history from one sample. See `gi_epochs.h`.

### `servers/rendering/renderer_rd/environment/rtgi.h`

`FrameInputs.hardware` stays false on this branch. Ray-query tracing arrives with the hardware rays work, and until then
no text may claim it.

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

A word of 0 means unknown. `next` wraps past 0 to 1. `fill_record` sets a flag only for a nonzero word, so a source with no
producer stays unknown in the record and fires no soft cut. Nothing in this commit produces an epoch.

The plain ray-scene hash can't drive a cut: `ray_scene_get_region` follows the camera unsnapped and `material_offset` is
a running count, so one instance crossing the region's edge changes the hash. In a populated exterior that restarts
history on most moving frames.

With no producer, scene, light and sky stay unknown, so R2's history lags a real scene or lamp change.

### `servers/rendering/renderer_rd/environment/gi_compositor.cpp`

The early-out also tests `RtgiPass::is_enabled`, so R2 on with `compositor/extra_provider` off keeps the textures R2 just
wrote, and the state reads `GI_COMPOSITOR_SLOT_DISABLED`. The slot-on and slot-off pair compares that way.

`ensure_slot_textures` makes the `ViewportState` first, because R2 runs before `_process` and the early-out erases it
while R2 is off. `publish` and `get_record` return quietly with no state, so `get_custom_data`'s error path never prints.

The AO relief for the slot's light isn't here. The slot's light takes full SSAO and multi-bounce AO.

### `servers/rendering/renderer_rd/environment/ddgi.cpp`

`MODE_RTGI` is `TRACE_GROUP_RTGI`, disabled at boot, because validity is all or nothing within a `ShaderRD` group: in group 0 a
failed R2 compile would take the probe, debug-ray and surfel traces with it, with R2 off. `DDGI::init` skips its pipeline, since
the disabled group's shader is a placeholder. `DDGI::trace_rtgi` enables the group and makes the pipeline on first use.

With R2 on, a failed R2 compile crashes after printing the glslang error instead of refusing, because `valid` stays true over the
emptied `variants`. R2-off runs don't compile the block, so a change elsewhere can break it unseen: compile `MODE_RTGI` at every
engine re-sync. The shader baker skips a disabled group.

### `servers/rendering/rendering_server.cpp`

| Internal setting | Default | Meaning |
|---|---|---|
| `rtgi/gpu_counters` | false | counters on, read at lazy start only |
| `rtgi/debug_status_path` | "" | JSON of `RtgiPass::get_status`, rewritten each frame while set |
| `rtgi/debug_view` | 0 | 1 tile class, 2 ray validity, 3 contribution |
| `rtgi/debug_dump_path` | "" | the debug view as raw RGBA16F at trace size, plus `<path>.txt`, `width height view` |

In the validity view a rejected pixel is raw (1.0, 0.5, 0.0). In the contribution view red is coverage, raw linear.

No public status API and no debug-draw enum exist, so nothing draws these views in the editor.

### `tests/servers/rendering/test_gi_epochs.cpp`

Covers the epoch rules. A publish after the early-out erased the state can't be tested without a device, because
`RenderSceneBuffersRD`'s destructor frees samplers through `MaterialStorage`.
