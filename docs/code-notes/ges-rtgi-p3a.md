# GI compositor slot: one extra provider over the DDGI stack

### `servers/rendering/renderer_rd/environment/gi_compositor.cpp`

`GICompositor::process` is called at the end of `GI::process_gi`, after the GI dispatch and inside its label. With
`compositor/extra_provider` and `compositor/debug_provider` both false it returns at once, and frees what an earlier
frame left in this viewport. A viewport that stops reaching `GI::process_gi` keeps those textures until its buffers are
configured again. No shader or pipeline exists until the first frame with one of the settings on, so a run with the slot
off compiles and binds nothing new.

The slot is a separate compute pass instead of a `USE_GI_EXTRA` variant of `gi.glsl`:

- `GI::process_gi` builds set 0 from the base variant and binds it for every mode, so bindings declared in set 0 only by
  the slot variants wouldn't match it. A variant would need a set of its own.
- No existing shader's text changes, so every existing GI variant keeps its SPIR-V by construction. The hunk in `gi.cpp`
  is the include, the teardown call, the usage bit for the dump and the one call to `process`.

What it costs: the pass reads the ambient texel after the GI dispatch stored it, so a covered texel's lower term has been
through float16 once more than inside `gi.glsl`, and can differ from that form by one float16 step. It adds one compute
dispatch per view with the slot on.

`GICompositor::_dump_ambient`: with `compositor/debug_dump_path` set and one of the two settings on, each frame writes
`<path>.pre` (ambient before composition), `<path>` (after) and `<path>.txt` (width, height, slot state as a number). Two
dumps of the same frame let a checker compare against the real lower stack. With both settings off nothing is written.
The path must be set before the viewport's first GI frame, because `GI::process_gi` adds the copy-from usage bit when it
creates the ambient texture. `GICompositor::_ambient_can_copy` skips the dump with one warning otherwise.

The dump reads back in the middle of the frame. That flush bumps `RenderingDevice::frames_drawn`, which `MultiUmaBuffer`
uses to detect a new frame, so with two 3D viewports or a reflection probe in one frame a buffer's ring can advance twice
and the next frame can write a slot the GPU still reads. Use the dump with one 3D viewport and no reflection probe.

### `servers/rendering/renderer_rd/environment/gi_compositor.h`

`GICompositor::ViewportState`: `written` belongs to the viewport, not to the process. A global "published since last
time" serial would let viewport A's provider switch on viewport B's slot, and B's textures were never written. It's
cleared after every composition, so a provider that stops turns the slot off on the next frame.

`gi_compositor_slot_state`:

- NOT_AVAILABLE: nothing written this frame in this viewport, the source isn't the slot's source, the kind isn't
  incident radiance, or the units differ from the lower stack's.
- DISABLED: all of that holds, the setting is off. The debug provider can run this way, and the lower stack stays as it
  was.
- ENABLED: all of that holds, the setting is on.

### `servers/rendering/renderer_rd/environment/gi_provider.h`

`gi_provider_make_record` leaves the record's epoch words at 0 with their flags clear. Nothing on dev produces them yet,
so claiming them would be false.

### `servers/rendering/renderer_rd/shaders/environment/gi_compositor_inc.glsl`

`gi_compositor_blend`. Lower stack (rgb_b, a_b), provider radiance p, coverage clamped above at 1 to c:

- Coverage NaN or <= 0: the lower stack unchanged.
- a_out = a_b + c - a_b c. It's at least c for a_b >= 0. If it isn't above 0, which takes a negative or NaN lower alpha
  and nothing on dev writes one, the lower stack unchanged.
- Otherwise rgb_out = (rgb_b a_b (1 - c) + p c) / a_out.

For a_b in 0..1 the two rgb weights are positive and add up to 1, so rgb_out is a convex mix of rgb_b and p. It can't
exceed max(rgb_b, p) in any component and the slot adds no energy. At a_b = 0 the output is p with alpha c. The slot sits
above DDGI, surfels, voxel and SDFGI, and it doesn't reach the reflection buffer.

### `servers/rendering/renderer_rd/shaders/environment/gi_extra_compose.glsl`

`main` skips the store when `!(coverage > 0.0)`, so a NaN or negative coverage leaves the texel's bytes alone.

### `servers/rendering/rendering_server.cpp`

| Internal setting | Default | Meaning |
|---|---|---|
| `compositor/debug_provider` | false | writes the synthetic provider each frame |
| `compositor/debug_pattern` | 0 | 0 constant, 1 ramp in x, 2 checker of 8 by 8 cells, coverage on even cells |
| `compositor/debug_coverage` | 1.0 | the pattern's coverage |
| `compositor/debug_radiance` | 0.0 | grey radiance |
| `compositor/debug_dump_path` | "" | see `GICompositor::_dump_ambient` |

### Left for the first real provider

`gi_epochs.*`, `gi_module_hooks.*`, `gi_provider_inc.glsl`, a provider registry, `gi_compositor_coverage` and
`gi_provider_confidence_from_samples` aren't here, because nothing here consumes them. `GICompositor::_run_synthetic` is
the only writer of `ViewportState`, and `_process` makes the textures after the GI dispatch. A real provider needs the
textures made before it runs and a public call to publish its record.

### Limits

- `GICompositor::process`: the slot needs `GI::process_gi` to run, so with no GI method on it does nothing.
- `GICompositor::_process`: with `extra_provider` on and no provider, the two textures stay allocated, 10 bytes per texel
  per view. The first frame with a setting on compiles the shaders and pipelines and can stall.
- `GICompositor::_dump_ambient`: reads layer 0 only. Multiview loops over views like the GI dispatch and isn't tested.
- `gi_extra_compose.glsl`: with VRS on, the GI dispatch writes coarse texels and the compose pass works per texel.
- `gi_extra_compose.glsl`: a provider's NaN or infinite radiance would reach the ambient buffer. The provider owns that
  guard.
- `scene_forward_clustered.glsl`: the slot's light is in `buffer_ambient`, so it enters `bounce_gi_light` and with it the
  screen-space bounce source of the next frame. It also takes full SSAO and multi-bounce AO, because the relief at
  `ambient_ao` follows surfel coverage only.
- `tests/servers/rendering/test_gi_compositor.cpp` covers the pure helpers. The default-off path needs a rendering device
  and has no unit test.
