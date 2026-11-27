# DDGI in the volumetric fog

Settings: `rendering/global_illumination/ddgi/volumetric_fog` (true),
`.../volumetric_fog_visibility` (true), `.../volumetric_fog_directional` (false). Defaults live in `fog_ddgi.h` and
`RenderingServer::init` registers them from there.

## What DDGI stores and what the fog reads

### `servers/rendering/renderer_rd/environment/ddgi.cpp`, `shaders/environment/ddgi.glsl`

- The irradiance atlas holds I(n) = E(n)/pi, the radiance-equivalent irradiance, not E. Its alpha is the update count,
  capped at 255; `MODE_RESET_COUNTS` sets it to 0.
- Exposure is already in the stored radiance (the trace multiplies emission and sky by it). The fog applies no
  exposure factor to DDGI light, unlike SDFGI and VoxelGI.
- Level origins in `DDGIData` are camera-relative. The fog's `mat3(params.cam_rotation) * view_pos` is in the same space.
- `Volume::mean` (RGBA16F, `level_count` layers, one texel per probe at `ddgi_probe_texel`) holds the mean ray radiance
  L_bar. `Volume::moment` (RGBA16F, `3 * level_count` layers, layer `level + c * level_count`, `ddgi_moment_texel`) holds
  the first moment f = mean of L_c w per colour, in world axes. Both are allocated and written whenever DDGI is on, so
  `Settings::same_storage` does not depend on the fog settings.
- `MODE_BLEND_IRRADIANCE`, thread 0 after the irradiance store: sums the valid rays (`ray.a >= 0`), reads the update
  count from shared `tile[0][0].a` (written before the barrier), builds the mean's history weight the same way as the
  irradiance texels, halves it on the same change test, and mixes both the mean and the three moments with that one
  weight. So a count reset resets the mean and f together, and |f| <= L_bar holds after every mix. f is not in the
  change test. Bindings 9 (mean) and 10 (moment) exist only in that mode; `_dispatch` adds them.
- Extra memory: mean 512 KiB and moment 1.5 MiB at the default 32 x 16 x 32 x 4 grid; 2 and 6 MiB at the largest.

## What each froxel gets

### `shaders/environment/ddgi_inc.glsl`

The old `DDGI_SAMPLING` block is four guarded pieces: `ddgi_sample_level` (`DDGI_SAMPLING`), `ddgi_level_coverage`
(`DDGI_SAMPLING` or `DDGI_MEDIUM_SAMPLING`), `ddgi_medium_level` and `ddgi_sample_medium` (`DDGI_MEDIUM_SAMPLING`), then the
surface entry points (`DDGI_SAMPLING`). The surface shaders see the same functions in the same order.

`ddgi_medium_level` is `ddgi_sample_level` without the normal and view bias and without the wrap weight (base 1). Under
`DDGI_MEDIUM_VISIBILITY` it applies the same Chebyshev test, cube, 0.5 and 0.2 crush and 1e-6 floor, with the distance
from the probe to the point. It reads the probe's mean texel, not the irradiance atlas. It returns the weight and a
confidence, `sum(t * v) / sum(t)` over the non-skipped corners: NONE and INSIDE corners are renormalised away and do not
lower it; hidden probes do. `ddgi_sample_medium` blends levels with `share = coverage * confidence * (1 - a)`, so a hidden
level gives way to a coarser one and, at the end, to the stock ambient.

### `shaders/environment/volumetric_fog_process.glsl`

Everything is inside `#ifdef ENABLE_DDGI`, in the light-loop branch: the stock ambient gets `* (1 - a)` and the fog adds
`L * a * ddgi.energy * params.gi_inject`. The result is multiplied by the same `scattering` as every other light, so it
must not be scaled by albedo or density again. Behind walls (a about 0) the froxel gets exactly the stock ambient.

Under `DDGI_MEDIUM_DIRECTIONAL`, L is `max(0, L_bar + 3 g (V . f))` per channel, with V = `safe_normalize` of the
camera-relative froxel position and g = `params.phase_g`. That is the band 0 and band 1 part of any phase with mean cosine
g; bands 2 and up are dropped. The clamp matters for |g| > 1/3. With g = 0 the C++ side picks the non-directional object,
so nothing of this runs.

## Never count GI twice

### `servers/rendering/renderer_rd/environment/fog.cpp`, `fog_ddgi.h`

`fog_gi_source` makes the whole GI choice before the compute list opens. DDGI wins when the setting is on, the volume
exists, object 0 has not failed and `gi_inject > 0.0001`; then SDFGI is not bound and `max_voxel_gi_instances` is 0, as on
surfaces. Otherwise `bind_sdfgi` and `voxel_gi_count` are the stock expressions, float against double literal, so the
stock band `0.0001 < gi_inject <= 0.001` (SDFGI without VoxelGI) is kept.

The DDGI variants live in two extra `VolumetricFogProcessShaderRD` objects, `ddgi_shader[0]` (plain) and `[1]`
(`DDGI_MEDIUM_DIRECTIONAL`), so a failed compile frees only that object's variants, never the stock fog. Each group
holds two variants, visibility off and on, with flat index `2 * _get_fog_variant() + visibility`
(`fog_ddgi_variant`). `_ensure_ddgi_shader(d)` runs on the first frame that wants object d: `initialize`, `enable_group`,
`version_create`, then `version_is_valid`, which starts the compile and waits for it. After that the shader cache serves
it. A failure, of the compile or of a pipeline, sets the object to failed with one error line; object 1 then falls back
to object 0, and object 0 to the stock path, in the same frame.

Set 0 is the stock density set, valid because the DDGI variants declare the same set 0 (`Params` is unchanged). Set 1 is
`gather_medium_uniforms()` (UBO 0, probe 1, distance 3, mean 4, moment 5) through the uniform-set cache keyed on the DDGI
shader RID, so each object and variant gets its own entry and a reallocated volume rebuilds it. Unused bindings 3 and 5
are ignored by `uniform_set_create`.

Limits:

- No depth pre-pass means DDGI is never processed; an unprocessed volume reads as NONE and the fog gets the stock
  ambient, like surfaces.
- Probe rays ignore fog extinction; fog light does not feed back into the probes.
- The first frame that wants a DDGI object waits for its compile.

### `tests/servers/rendering/test_fog_ddgi.cpp`

The 4/N bound of the rotated band-3 mean test is the design's hand bound. A Python port of the same ray set (five fields,
1000 rotations each) gave a largest error of 0.22/N times max|L - L_bar|, and the largest |sum of w_r| over N = 32 to 256
was 0.165, against the 2.15 bound.
