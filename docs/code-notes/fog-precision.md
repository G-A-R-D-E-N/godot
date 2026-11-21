# Volumetric fog precision and path length

### `servers/rendering/renderer_rd/shaders/environment/volumetric_fog.glsl`

**Density scale.** `DENSITY_SCALE` sums density in signed 32-bit fixed point at 2^20 counts per unit (per metre).
- Stock used 1024, which made every density a multiple of 9.77e-4 and truncated toward zero, so 1.5e-3 read 35 % low.
- At 2^20 a step is 9.5e-7, and rounding error is at most half of that. That matches the fp16 density map's own half-ulp at 1e-3, so nothing finer would survive the next stage.
- `volumetric_fog_process.glsl` uses the same constant, and the two must change together.

**Density limit.** `DENSITY_LIMIT` clamps each volume's density to ±32 per metre before it is quantised.
- The sum is read back as a signed int32, so it holds just under 2048 per metre. 63 volumes at the limit fit, and a 64th wraps the froxel to empty.
- 32 per metre makes 1 cm of fog 27 % opaque, and FogMaterial's own range is ±8, so nothing real is cut.
- Values are rounded with `floor(x + 0.5)`, the same on every driver and in the CPU tests. Above 8 per metre (2^23 counts) a float32 tie rounds to even, which moves the result by one count (9.5e-7 per metre).

**Density gate.** The gate is `abs(density) > 0.0`. It is false for NaN, which keeps stock's implicit NaN guard, and it no longer drops densities at or below 0.001.

**Exact colour.** The setting `rendering/environment/volumetric_fog/exact_volume_color` allocates the light map 4x deep and the emission map 3x deep.
- The shaders read the mode from the map itself, `imageSize(light_only_map).z > fog_volume_size.z`, so they always agree with what was allocated.
- Params, ParamsUBO and the bindings do not change, so every variant built from the same source follows without an edit, the DDGI fog variant included.

Layout:
- Channel c of froxel (x, y, z) is texel (x, y, 4z + c) in the light map and (x, y, 3z + c) in the emission map.
- Light words 0 to 2 hold clamp(albedo, 0, 1) × density per channel.
- Light word 3 holds the positive density sum. It is written for every volume whose density rounds to a positive count, whether or not the material uses albedo, and it is the same count the density word gets. The colour words are written only for those volumes too, so a froxel never holds colour without the density that carries it.
- Emission words hold max(emission, 0) × density per channel, clamped to 64 per volume.
- Each colour channel is added only when its amount is above 0, which skips black channels and is false for NaN.
- All six colour words are unsigned, at the density scale. They hold just under 4096 per metre, so 63 volumes at 64 fit.
- The net density word is signed and wraps first, at 64 volumes at the density limit. The froxel is then empty, because `carve` is 0 while the positive word still holds a large value.

There is no density clamp to 0..1, no emission cap of 4 and no packing, so the blue-into-green carry of the packed format cannot happen.

**Exact colour off.** The stock packed path runs as stock text.
- Its gate stays `abs(density) > 0.001`, so the light and emission words are stock word for word.
- Clamping density to ±32 first changes nothing there, because the packed path clamps to 0..1 anyway.

**Buffer path.** Exact colour needs image atomics.
- The NO_IMAGE_ATOMICS buffer path, used only by Metal on older OS versions, keeps packed colour.
- Godot's Metal driver binds no buffer-size buffer, so a `.length()` mode test would have nothing to read there.

### `servers/rendering/renderer_rd/shaders/environment/volumetric_fog_process.glsl`

**Carve fraction.** In the MODE_DENSITY decode, exact colour scales scattering and emission by `carve = min(1, total_density / (base_density + positive_density))`. That is the fraction of the medium left after negative volumes carve it.
- total_density is already max(0, base + net).
- With no negative volumes, net and positive are the same integer sum, so the fraction is exactly 1.
- A carve-out therefore removes light in proportion and keeps the albedo, base fog included. Stock left the scattering in, so a carved region could exceed albedo 1.

**Light gate.** The light loop runs for `total_density > 1e-6`.
- Stock used 5e-5, which left thin fade shells with extinction but no light.
- One density step is 9.5e-7, so a single rounding remnant still skips the loop.

**Path factor.** In MODE_FOG, `path_scale` turns the view-depth step between slice centres into the path length. Stock used the depth step, so off-axis extinction was low by cos(theta).
- A froxel column is a straight line in view space whatever the detail spread: x = n × (begin + (end - begin) t), z = -fog_end × t.
- So each column has one exact factor, `sqrt(1 + |n × (end - begin) / fog_end|^2)`, taken at the column centre.
- At the 16:9 corners it is 1.380, 1.545 and 1.857 for vertical FOV 50, 60 and 75 degrees. Orthographic cameras have begin = end, so it is 1.
- For perspective it is within 0.06 % of the eye ray's factor for fog length 64 m or more with a near plane of 0.05 m, because the grid starts at the near-plane size, not at the eye.
- The density pass does not use it: that pass works on jittered positions and feeds the history.
- The sky reads the same fog map, so it gets the factor with no change. The scene reader is depth-indexed and needs none.

**In-scatter weight.** `in_scatter` replaces stock's divide by `max(a, 1e-5)`, which made the in-scatter low by a / 1e-5 below 1e-5 per metre.
- For tau above 0.05 it is the exact `(1 - T) / a`, written as stock's expression.
- Below that it is the series `d (1 - tau/2 + tau^2/6 - tau^3/24)`. At the switch the series is off by about tau^4/120 = 5.2e-8, while the direct form's float32 error is about 1.2e-6.
- It is 0 when a is 0, as in stock, so emission in empty air adds nothing.
- The accumulated transmittance from before the step is used, as in stock.

### `servers/rendering/renderer_rd/environment/fog.cpp`

**`Fog::VolumetricFog::init`** keeps requested and active apart.
- `exact_color_requested` is what the validate check in `RenderForwardClustered::_update_volumetric_fog` compares with the setting, so changing the setting re-creates the volume once.
- `exact_color_active` is decided here. It is false on the buffer path, or when 4 times the depth exceeds `LIMIT_MAX_TEXTURE_SIZE_3D`. So a fallback warns once and does not re-create the volume every frame.
- Only the light and emission maps get the extra depth. The density map and the history clamp map keep theirs.

### `servers/rendering/renderer_rd/environment/fog_precision.h`

**`RendererRD::FogPrecision`** is the CPU form of the shader maths, used by `tests/servers/rendering/test_fog_precision.cpp`. It covers:
- quantising and decoding with the same rounding;
- the carve fraction;
- the light gate;
- the path factor;
- the in-scatter weight.

The word counts for the deeper maps come from here too. The shaders cannot include it, so any change to one must be mirrored in the other.
