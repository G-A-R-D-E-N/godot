# Local exposure and the histogram meter highlight policy

Two features share this note.

- Local exposure adapts exposure per screen region. A compute pass builds a low-resolution, edge-aware base of the
  scene's log luminance. The tonemapper then brightens or darkens each pixel by a bounded number of stops. The shift
  depends on the base and on the pixel's own luminance.
- The histogram meter highlight policy changes the target of the log histogram auto exposure meter. The bright end of
  the frame then still counts after the trimmed mean.

Local exposure is off unless `Environment.local_exposure_enabled` is set and `local_exposure_strength` is above 0. While
it is off, the image is exactly the globally exposed one. The highlight policy is off while
`auto_exposure_highlight_percentile` is `0.0`.

The auto exposure exponential response, light adaptation limit and hold travel in the same structs and push constants.
They are described at the end of the histogram section.

### Frame order

All of it runs inside `RendererSceneRenderRD::_render_buffers_post_process_and_tonemap`, in this order:

1. Depth of field, when enabled. It writes into the internal texture.
2. Auto exposure (label "Auto Exposure"). The meter reads the internal texture and writes the one-texel luminance
   buffer that the tonemapper reads.
3. Glow, when enabled. It writes its own blur textures and leaves the internal texture unchanged.
4. Local exposure (label "Local Exposure", `RENDER_TIMESTAMP("Local Exposure")`).
5. Tonemap. The tonemap shader reads the local exposure textures and applies the gain.

The meter and the local exposure base both read the internal texture before any local gain exists. Local exposure
therefore never feeds back into auto exposure, and the base contains no glow.

Local exposure runs only when all of these hold:

- `RendererSceneRenderRD::local_exposure` is not null. `RendererSceneRenderRD::init` creates it only when
  `_render_buffers_can_be_storage()` is true, that is, for a renderer that can write storage images (Forward+). The
  Mobile and Compatibility renderers never create it. The `RendererRD::LocalExposure` constructor registers the
  project settings, so they exist only where the pass exists.
- `can_use_effects` is true. That needs a target of at least 8 by 8 pixels and a debug draw mode that keeps camera
  effects (`_debug_draw_can_use_effects`). The modes that replace the whole image turn effects off: unshaded, overdraw,
  wireframe, VoxelGI albedo, the cluster views, internal buffer, persistent AO and the fallback source views.
- `can_use_storage` is true. The condition tests it next to the null check, and both mean the same thing.
- `rb->get_view_count() == 1`. Stereo and multiview viewports skip the pass.
- The viewport has an environment.
- `environment_get_local_exposure` returns `enabled` with `strength > 0.0`.

When any condition fails, no pass is dispatched, no buffers are allocated and the tonemapper runs with
`TONEMAP_FLAG_USE_LOCAL_EXPOSURE` clear. The pass does not depend on camera attributes. A viewport without camera
attributes still gets local exposure on top of the exposure it has.

With a temporal upscaler (`use_upscaled_texture`) the tonemapper reads the upscaled texture, while the pass meters
`rb->get_internal_texture()` at `rb->get_internal_size()`. The grid and the `wide` texture are addressed by normalised
`uv`, so the two sizes may differ.

### Settings

#### Environment (per scene)

`RendererEnvironmentStorage::environment_set_local_exposure` stores these values. Every limit passes through
`servers/rendering/storage/local_exposure_limits.h` (namespace `LocalExposureLimits`).

| Property | Default | Stored range | Function |
|---|---|---|---|
| `local_exposure_enabled` | `false` | | `Environment::set_local_exposure_enabled` |
| `local_exposure_strength` | `1.0` | 0 to 1 | `LocalExposureLimits::strength` |
| `local_exposure_shadow_contrast` | `0.7` | 0.25 to 1 | `LocalExposureLimits::contrast` (`MIN_CONTRAST = 0.25`) |
| `local_exposure_highlight_contrast` | `0.8` | 0.25 to 1 | `LocalExposureLimits::contrast` |
| `local_exposure_max_shadow_lift` | `2.0` stops | 0 to 6 | `LocalExposureLimits::shift` (`MAX_SHIFT_STOPS = 6`) |
| `local_exposure_max_highlight_drop` | `1.5` stops | 0 to 6 | `LocalExposureLimits::shift` |
| `local_exposure_pivot` | `0.0` stops | -8 to 8 | `LocalExposureLimits::pivot` (`MAX_PIVOT_STOPS = 8`) |

`Environment::_update_local_exposure` sends all seven values to `RenderingServer::environment_set_local_exposure` on
every property change. The inspector ranges equal the stored ranges. A value past a range is clamped. A NaN in any of
the six floats makes `environment_set_local_exposure` log an error and return before it changes anything, so the
previous values, including `enabled`, stay.

The call travels through `RenderingServerDefault` (`FUNC8`) and `RendererSceneCull` (`PASS8`) to
`RendererSceneRender::environment_set_local_exposure`, which forwards it to `environment_storage`. The Compatibility
renderer derives from `RendererSceneRender`, so it stores the same values without any pass behind them.
`RendererEnvironmentStorage::LocalExposure` is the plain struct that `environment_get_local_exposure` returns. It is not
the `RendererRD::LocalExposure` pass class.

#### Project settings (global)

The `RendererRD::LocalExposure` constructor registers these, and the renderer reads them each frame with
`GLOBAL_GET_CACHED`.

| Setting | Default | Range or values | Use |
|---|---|---|---|
| `rendering/environment/local_exposure/method` | `0` | `0` Bilateral Grid, `1` Edge-Checked Blur | Passed to the tonemap shader as `method`. Values above 1 become 1. |
| `rendering/environment/local_exposure/spatial_sigma` | `1.0` | 0.25 to 2 | Blur width across the screen, in grid cells. `LocalExposure::process` clamps it to 0.25 to 2. |
| `rendering/environment/local_exposure/range_sigma` | `1.0` | 0.1 to 1.5 | Blur width along the luminance axis, in stops. Clamped to 0.1 to 1.5. |
| `rendering/environment/local_exposure/wide_blend` | `0.0` | 0 to 1 | Bilateral Grid only. Mix of the per-region mean into the base. Clamped to 0 to 1. |
| `rendering/environment/local_exposure/edge_range` | `1.0` | 0.1 to 6 | Edge-Checked Blur only. Distance in stops at which a pixel starts to follow its own luminance. Clamped to 0.1 to 6. |
| `rendering/environment/local_exposure/temporal_seconds` | `0.0` | 0 to 2 | Time constant of the temporal filter. A value of 0 or less turns it off. The range is an editor hint. The code does not clamp a larger value. |
| `rendering/environment/local_exposure/pivot_source` | `0` | `0` Fixed, `1` Frame Median | Where the unchanged level comes from. Any value other than 1 selects Fixed. |

### Buffers: `RendererRD::LocalExposure::Buffers`

`LocalExposure::get_buffers` returns the `Buffers` object stored in the render buffers under the name
`RB_LOCAL_EXPOSURE_BUFFERS` ("local_exposure_buffers"). It creates the object on first use. The class derives from
`RenderBufferCustomDataRD`, so each render buffer set (each viewport) has its own buffers and its own history.

`Buffers::configure` sizes everything from the internal size of the render buffers:

- `log_size = max(ceil(internal_size / LOG_DIVISOR), 1)` per axis, with `LOG_DIVISOR = 8`.
- `grid_size = max(ceil(log_size / GRID_DIVISOR), 1)` per axis, with `GRID_DIVISOR = 4`.
- One grid cell covers about 32 by 32 pixels of the internal image. For a 1920 by 1080 internal size, `log_size` is
  240 by 135 and `grid_size` is 60 by 34.

| Member | Format and size | Content |
|---|---|---|
| `log_luminance` | `R32_SFLOAT`, `log_size` | Mean log2 luminance per 8 by 8 block |
| `grid[2]` | `R16G16B16A16_SFLOAT`, 3D, `grid_size` by `GRID_DEPTH` (64) | Per cell and luminance bin: `r` weighted sum of log luminance, `g` weight |
| `history[2]` | `R32G32B32A32_SFLOAT`, 3D, same size | Temporal result, same layout as `grid` |
| `wide` | `R16G16B16A16_SFLOAT`, 2D, `grid_size` | `r` per-cell mean log luminance over all bins |
| `median` | `R32_SFLOAT`, 1 by 1 | Frame median log luminance, or `1e30` for none |

All of them use storage and sampling usage. `LOG_MIN = -20.0` and `LOG_MAX = 16.0` give the log2 luminance range that
the 64 bins span. A bin is 36 / 64 = 0.5625 stops wide. The grid holds half-precision sums, which limits their
precision to about three significant digits. The history is 32-bit float. `median` is allocated for every `Buffers`
object, also when `pivot_source` is Fixed. It is written only on frames in which the median step runs.

`Buffers::free_data` frees every texture and clears `history_valid`. `RenderSceneBuffersRD::cleanup` calls it, and the
render buffers then call `configure` again on every reconfiguration (a resize, a scaling change, a reflection buffer
setup). A resize therefore gives new, correctly sized textures, `history_index = 0` and `history_valid = false`.
`last_frame` is not reset by either function.

Nothing is allocated until the first frame in which the pass runs. Disabling local exposure later leaves the buffers
allocated. A later reconfiguration of the render buffers allocates them again.

### Compute pass: `LocalExposure::process` and `local_exposure.glsl`

`process(p_source_texture, p_source_size, p_buffers, p_settings)` records every step in one compute list. It returns the
grid that the tonemapper should sample.

It returns an empty `RID` after an error message when the uniform set cache or material storage is missing, or when the
buffers are null or freed. The caller then skips local exposure for that frame. The buffers stay allocated.

The shader `local_exposure.glsl` has one `MODE_*` variant per step, all with an 8 by 8 local size. They share one push
constant block: `source_size`, `dest_size`, `axis` and `sigma`. A barrier follows each step that a later step reads.
Both `method` values run every step. Nothing is skipped per method.

`LocalExposure::Settings` carries `spatial_sigma`, `range_sigma`, `temporal_alpha`, `reset_history`, `frame_median` and
`frame`.

#### 1. `MODE_DOWNSAMPLE`: log luminance

One thread per `log_luminance` texel. It takes 16 linear-filtered taps from the 8 by 8 block, at offsets 1, 3, 5 and 7
in both axes from the block origin. Each tap sits on a pixel corner. The linear sampler therefore averages 2 by 2
pixels in linear colour, and the 16 taps cover the block once. A tap is skipped when its corner coordinate is greater
than `source_size` in either axis.

For each tap:

```
y = dot(rgb, vec3(0.2126, 0.7152, 0.0722))
y = y > LUMINANCE_FLOOR ? min(y, LUMINANCE_CEILING) : LUMINANCE_FLOOR
sum += log2(y)
```

`LUMINANCE_FLOOR` is 2^-20 (9.5367431640625e-07) and `LUMINANCE_CEILING` is 65504. NaN fails the comparison and
becomes the floor. Infinity becomes the ceiling. Every stored value is finite and inside `LOG_MIN` to `LOG_MAX`. The
texel holds the mean of the logs, not the log of the mean. It holds the log2 of the floor when no tap was valid.

The source is the internal texture of the render buffers. It holds scene values before exposure. The pass runs only
on renderers that can use storage images. On those, `RenderSceneBuffersRD::get_luminance_multiplier` returns 1.0, so the
internal texture needs no rescaling.

#### 2. `MODE_BUILD`: bilateral grid

One thread per grid cell. It writes all 64 bins of that cell into `grid[0]`. The shader reads `log_luminance` with
`texelFetch`.

- `scale = source_size / dest_size` is the real `log_size / grid_size` ratio. It is close to 4 and is not exactly 4
  when the size does not divide evenly.
- The thread covers the `log_luminance` texels within one `scale` of the cell centre.
- Each texel gets a tent weight `w = tent.x * tent.y` from its distance to the cell, in cell units.
- The texel's log value `l` becomes a bin position
  `p = clamp((l - LOG_MIN) / (LOG_MAX - LOG_MIN) * 64 - 0.5, 0, 63)`. It is split linearly between bins `floor(p)` and
  `floor(p) + 1` with fraction `f`.
- Each bin accumulates `sum += w * share * l` and `weight += w * share`, with `share` equal to `1 - f` or `f`.

The cell value used later is `sum / weight`, the weighted mean log luminance of that bin. The `-0.5` in `p` matches the
texel centres of the 3D texture that the tonemap shader samples.

#### 3. `MODE_BLUR`: three separable passes

Three dispatches over `grid_size` by 64 blur the grid along x, y and then luminance. Each pass is a 9-tap Gaussian
(`BLUR_RADIUS = 4`) with `sigma = max(params.sigma, 0.05)`. It reads only the `rg` channels, with `texelFetch`,
and writes `rg` with `ba` set to 0.

| Pass | Axis | Source | Destination | Sigma |
|---|---|---|---|---|
| 0 | x | `grid[0]` | `grid[1]` | `spatial_cells = clamp(spatial_sigma, 0.25, 2)` |
| 1 | y | `grid[1]` | `grid[0]` | `spatial_cells` |
| 2 | luminance | `grid[0]` | `grid[1]` | `range_cells = clamp(range_sigma, 0.1, 1.5) * 64 / (LOG_MAX - LOG_MIN)` |

`range_cells` converts a width in stops to a width in bins. The kernel normaliser adds the weight of every tap, also for
taps outside the grid. Values near an edge are therefore scaled down. Both channels scale equally, so the ratio
`sum / weight` is unchanged there.

After pass 2, `grid[1]` holds the fully blurred grid. After pass 1, `grid[0]` holds a grid blurred in x and y only.

#### 4. `MODE_TEMPORAL`: optional smoothing

`temporal_alpha = temporal_seconds > 0 ? 1 - exp(-time_step / temporal_seconds) : 1`. `time_step` is the frame step in
seconds that `RendererSceneRenderRD::set_time` stored. The step runs only when `temporal_alpha < 1.0`.

- It reads `grid[1]` (now) and `history[history_index]` (before). It writes `history[1 - history_index]`.
- The blend factor is `temporal_alpha` while `history_valid` is true and `reset_history` is false. Otherwise it is
  `1.0`, which copies the current grid and restarts the filter.
- The shader blends the `rg` pair with `mix(before, now, sigma)` only when `sigma < 1`. Sum and weight are blended
  alike, so the ratio stays a weighted mean.
- Afterwards `history_index` flips and `history_valid` becomes true. The result is `history[history_index]`.
- With a valid history and no reset, a zero `time_step` gives `temporal_alpha = 0`, and the previous history carries
  over. With an invalid history or a reset, a zero `time_step` still copies the current grid.

Without the step, the result is `grid[1]` and `history_valid` is cleared. The step is skipped when `temporal_seconds`
is 0 or less. It is also skipped when `temporal_alpha` reaches 1.0, for example after a very long frame.

The history is invalid at the start of a call when `p_settings.frame != last_frame + 1`. `frame` comes from
`RSG::rasterizer->get_frame_number()`. `last_frame` is the frame of the previous call to `process` for this render
buffer. Any frame in which this buffer did not run local exposure restarts the filter. Examples are a disabled
environment, a skipped viewport and a paused viewport.

`reset_history` is `rb->get_history_reset()`. `RenderSceneBuffersRD::update_history_reset` refreshes it once per frame.
It is true when the project setting `rendering/anti_aliasing/quality/history_reset_serial` changed since the previous
frame, which is how a game signals a cut. It is also true when the exposure normalisation factor changed. That factor
is 1.0 unless physical light units and camera attributes are in use.

#### 5. `MODE_WIDE`: per-cell mean

One thread per cell. It adds the `rg` pair over all 64 bins of `base_grid` and writes
`total.g > 1e-5 ? total.r / total.g : log2(LUMINANCE_FLOOR)` into `wide`. The shader reads with `texelFetch`.

`base_grid` is the grid that this step and the median step read:

- With temporal smoothing it is `result`, the temporal output. That output was built from the fully blurred `grid[1]`.
- Without temporal smoothing it is `grid[0]`, the grid blurred in x and y only.

The blur along luminance keeps the total weight and sum, except for the share that falls past the first and last bin.
`wide` is therefore the same in both cases apart from the two ends of the luminance range. The median step reads the
same `base_grid`. A frame without temporal smoothing takes its median from a grid that is not blurred along luminance.

In method 1 the result depends on `spatial_sigma` through `wide`. `range_sigma` changes it only at the two ends of the
luminance range.

#### 6. `MODE_MEDIAN`: frame median (only with `pivot_source = 1`)

The step runs when `Settings::frame_median` is set and `median` is valid. One workgroup of 8 by 8 = 64 threads is
dispatched. Thread `z = gl_LocalInvocationIndex` owns bin `z`.

Each thread adds the `rg` pair of every cell `(x, y)` at its own `z` into `total`.

- `MEDIAN_BATCH = 8`. In every row, `batched = dest_size.x - dest_size.x % 8` cells are read in groups of eight. The
  eight reads of a group are fetched first (`c0` to `c7`) and then added. The remaining cells of the row are read one
  by one.
- The additions keep the order of a plain loop: `x` ascending, row after row. `total` is declared `precise`, so the
  compiler may not reorder them.
- The batch only lets the eight reads of a group be issued together. The sum equals the sum of a one-cell loop.
- The thread stores `bin_sum[z]` and `bin_weight[z]` in shared memory. A `barrier()` follows.

Thread 0 then computes the median:

1. `all` is the sum of `bin_weight`. If `all <= 1e-5`, the result is `MEDIAN_NONE` (`1.0e30`).
2. `half_weight = 0.5 * all`. Bins with no weight are skipped.
3. For each remaining bin, `mean = bin_sum / bin_weight` and `middle = running + 0.5 * bin_weight`. `running` is the
   weight of the earlier bins. The bin's weight is then added to `running`.
4. At the first bin whose `middle >= half_weight`:
   - If it is the first non-empty bin, the median is its `mean`.
   - Otherwise the median is
     `previous_mean + (half_weight - previous_middle) / (middle - previous_middle) * (mean - previous_mean)`. This is a
     linear interpolation between the means of the two neighbouring non-empty bins at the half-weight point.
5. If the loop ends without reaching `half_weight`, the median is the mean of the last non-empty bin.

The interpolation lets the median move continuously when weight shifts between bins. It is not limited to the 64 bin
means. `LocalExposureLimits::median_log` in `local_exposure_limits.h` is a header-only copy of the same procedure. It
takes weight and sum arrays and a "none" value. The shader does not call it. The unit tests in
`tests/servers/rendering/test_local_exposure_limits.cpp` use it as the reference for the cases above.

The result goes to the 1 by 1 `median` texture. No barrier follows, because the compute list ends.

### Tonemap integration

#### `ToneMapper::tonemapper` (`tone_mapper.cpp`, `tone_mapper.h`)

`TonemapSettings` has the fields `use_local_exposure`, `local_exposure` (the `RendererEnvironmentStorage::LocalExposure`
struct), `local_exposure_grid`, `local_exposure_wide`, `local_exposure_median`, `local_exposure_method`,
`local_exposure_pivot_source`, `local_exposure_wide_blend` and `local_exposure_edge_range`.
`RendererSceneRenderRD` fills them after the pass.

- `local_exposure_grid` is the `RID` that `process` returned. That is `history[history_index]` (RGBA32F) with temporal
  smoothing, and `grid[1]` (RGBA16F) without it. The code does not check whether the device filters RGBA32F linearly.
- `local_exposure_pivot_source` is 1 only for the Frame Median setting. `local_exposure_median` is the `median` texture
  in that case and an empty `RID` otherwise.
- `use_local_exposure` inside `tonemapper` is `settings.use_local_exposure && view_count == 1` with both grid and wide
  textures valid. It then sets `TONEMAP_FLAG_USE_LOCAL_EXPOSURE` (`1 << 9`, `FLAG_USE_LOCAL_EXPOSURE` in the shader)
  and writes `LocalExposureUniforms` (std140, 48 bytes) to the uniform buffer `tonemap.local_exposure_uniforms`. The
  `ToneMapper` constructor creates that buffer once, for the non-mobile variant only.

| `LocalExposureUniforms` field | Value |
|---|---|
| `strength` | `local_exposure.strength` |
| `shadow_slope` | `shadow_contrast - 1` |
| `highlight_slope` | `highlight_contrast - 1` |
| `max_lift`, `max_drop` | `max_shadow_lift`, `max_highlight_drop` |
| `pivot_log` | `log2(0.18) + pivot` |
| `wide_blend`, `edge_range` | the two project settings |
| `method` | `0` or `1` |
| `pivot_source` | `1` if Frame Median is selected and the median texture is valid, else `0` |
| `pivot_offset` | `pivot` |
| `pad` | `0` |

- Descriptor set 4 is always bound: the 3D grid at binding 0, the wide texture at binding 1, the uniform buffer at
  binding 2 and the median texture at binding 3. The grid and wide textures use the linear sampler with no repeat, so
  lookups clamp at the texture edges. The median uses the nearest sampler. When local exposure is off, the grid slot
  gets the default white 3D texture and the wide and median slots get the default white 2D texture. The pipeline layout
  stays valid, and the shader does not read the slots without the flag.
- The mobile variant (`ToneMapper` constructed with `p_use_mobile_version`) and its shader have no local exposure.
  `tonemapper` refuses to run on that variant. `RendererSceneRenderRD` calls `ToneMapper::tonemapper_mobile` when
  `can_use_storage` is false.

#### `local_exposure_gain` in `tonemap.glsl`

The tonemap `main` first applies `luminance_multiplier` and exposure. It then calls
`local_exposure_gain(color.rgb, uv_interp, exposure)` when the flag is set, with the pixel as exposed and before FXAA
and glow. The metering pixel is therefore the one that is not antialiased and has no glow.

The scalar gain is applied later, just before `apply_tonemapping`. It comes after FXAA, after the pre-tonemap glow and
after `apply_night_vision`. With night vision on, it comes before `apply_night_highlights`. The gain multiplies all
three channels by the same factor, so hue and saturation are kept. It scales the pre-tonemap glow that was added. It
does not touch the `SOFTLIGHT` glow, which is added after `apply_tonemapping`. Brightness, contrast, saturation and
colour correction run after the gain.

```
scale     = max(exposure, 1e-20)
luminance = dot(exposed, (0.2126, 0.7152, 0.0722)) / scale
own       = log2(clamped luminance)            same floor and ceiling as the pass
wide      = texture(source_local_wide, uv).r
```

Both lookups use the linear sampler. The grid is interpolated across cells and trilinearly along luminance.

Base, by `method`:

- `0` (Bilateral Grid): `cell = texture(source_local_grid, (uv, (own - LOG_MIN) / (LOG_MAX - LOG_MIN))).rg`.
  `base = cell.g > 1e-4 ? cell.r / cell.g : own`, then `base = mix(base, wide, wide_blend)`. A lookup with a weight of
  1e-4 or less falls back to the pixel's own log luminance.
- `1` (Edge-Checked Blur): `edge = smoothstep(edge_range, 2 * edge_range, abs(own - wide))` and
  `base = mix(wide, own, edge)`. Where the pixel is within `edge_range` of the regional mean, the base is the mean.
  Two `edge_range`s away or more, the base is the pixel itself.

The gain depends on the pixel's own luminance only through the base. Method 0 looks the grid up at the pixel's own
luminance bin. Method 1 blends `wide` toward the pixel's own log luminance.

Pivot:

- Fixed: `pivot = pivot_log = log2(0.18) + pivot`, a level in exposed units. The pivot setting is clamped to -8 to 8
  stops when it is stored.
- Frame Median: `median = texture(source_local_median, 0.5).r`. If `median < LOCAL_LOG_MAX` (16), then
  `pivot = median + log2(scale) + pivot_offset`. A median of `1e30` (no weight in the frame) fails that test, and the
  fixed pivot is used for that frame. The offset is clamped to the same -8 to 8 range, so the unchanged level can sit
  up to 8 stops either way from the frame median.

`stops = base + log2(scale) - pivot`. With the median pivot the exposure terms cancel and `stops = base - median -
pivot_offset`. A region whose base equals the frame median plus the offset keeps its global exposure. Local exposure then
only moves light between regions.

The shift in stops:

```
stops < 0 (dark region):    shift = max_lift * tanh(clamp(shadow_slope    * stops / max_lift, -10, 10))
stops >= 0 (bright region): shift = max_drop * tanh(clamp(highlight_slope * stops / max_drop, -10, 10))
gain = exp2(strength * shift)
```

A cap of 0 gives a shift of 0 for that side. The inner clamp to -10 and 10 keeps `tanh` finite.

A slope is `contrast - 1`, so it lies between -0.75 and 0. Near the pivot, the shift is about `(contrast - 1) * stops`.
Of the original distance to the pivot, `contrast * stops` remains. A contrast of 1 changes nothing. A lower contrast
pulls the region toward the pivot. The `tanh` saturates at `max_lift` for dark regions and at `-max_drop` for bright
regions.

### Histogram meter highlight policy

The policy belongs to the log histogram meter (`Luminance::luminance_histogram_meter`, shader
`luminance_histogram.glsl`). It needs the compute effects path. `Luminance` is constructed with
`prefer_raster_effects = !can_use_storage`, and `supports_histogram_meter()` returns `!prefer_raster_effects`.

`RendererSceneRenderRD` runs `Luminance::luminance_reduction` instead when the meter is not the log histogram or when
the histogram is not supported. For a log histogram meter that cannot run, it prints one warning ("The log histogram
auto-exposure meter needs the compute effects path; using the mean meter.") and ignores the policy.

The mean meter has no percentile policy. It keeps its own `highlight_limit` clamp (`luminance_reduce.glsl`,
`luminance_reduce_raster.glsl`, `luminance_reduce_raster_inc.glsl`). That clamp limits each sample to the previous
unadapted luminance times `2^limit`. It does not read `auto_exposure_highlight_percentile`,
`auto_exposure_highlight_ceiling` or `auto_exposure_highlight_max_shift`.

#### Settings (camera attributes)

| Property | Default | Stored range | Meaning |
|---|---|---|---|
| `auto_exposure_highlight_percentile` | `0.0` | 0 to 1 | Position in the cumulative weight at which the frame's bright level is read. 0.99 reads the level that 99 percent of the weight lies below. The brightest `1 - percentile` share is ignored. `0.0` is off. |
| `auto_exposure_highlight_ceiling` | `2.5` stops | 0 to 8 | How far above the metered target the protected level may sit. |
| `auto_exposure_highlight_max_shift` | `4.0` stops | 0 to 12 | The most the rule may move the target. |

`CameraAttributes::_update_auto_exposure_response` sends these, together with the response and guard values, to
`RenderingServer::camera_attributes_set_auto_exposure_highlight`.
`RendererCameraAttributes::camera_attributes_set_auto_exposure_highlight` clamps them with `CLAMP` to the ranges above.
The inspector ranges are the same.

`RendererSceneRenderRD` reads the stored values into `RendererRD::Luminance::HistogramPolicy`, an alias of
`LuminanceHistogramPolicy` in `luminance.h`. It is a struct of plain values, and `luminance_histogram_meter` copies it
into the push constants. A caller that passes no policy gets the defaults: percentile `0.0`, ceiling `2.5`, max shift
`4.0`, `exponential` false, `light_adjust` and `dark_adjust` `1.0`, `light_limit` `0.0` and `hold` false.

#### `ACCUMULATE` in `luminance_histogram.glsl`

The shader has two modes. `ACCUMULATE` fills a 256-bin histogram. A bin is `BIN_WIDTH = 0.25` log2 stops wide, and the
first bin starts at `MIN_LOG2 = -32`.

- It reads every `stride`-th pixel with `texelFetch`. `Luminance::histogram_stride` picks the stride so that at most
  `LUMINANCE_HISTOGRAM_MAX_SAMPLES` (2^21) pixels are read.
- A NaN luminance is skipped.
- Each sample gets the integer weight `floor(8 * (1 + 3 * clamp(center_weight, 0, 1) * g) + 0.5)`. `g` is a Gaussian
  centred at 0.5 of the width and 0.6 of the height, with a sigma of 0.3 of the height. With `center_weight = 0` every
  sample weighs 8.
- The bin position is `p = (log2(y) - MIN_LOG2) / BIN_WIDTH - 0.5`, clamped to 0 to 255. The weight is split between
  the two nearest bins in 32 steps (`TENT_STEPS`).
- Positive infinity goes to the last bin. Zero, negative and negative-infinite luminance go to the first bin.

#### `RESOLVE` in `luminance_histogram.glsl`

`RESOLVE` runs as one workgroup of 256 threads, one per bin.

1. A prefix sum gives each bin `below` (weight before the bin) and `above` (weight up to and including it). `total` is
   the weight of all bins, including the centre weighting.
2. The trimmed mean cuts the tails. `lo = clamp(clip_low, 0, 0.45) * total` and
   `hi = (1 - clamp(clip_high, 0, 0.45)) * total`. The kept part of each bin is
   `part = max(min(above, hi) - max(below, lo), 0)`. It gives `local_sum[t] = part * bin centre` and
   `local_kept[t] = part`.
3. The highlight position is found in the same pass. With `highlight_percentile > 0` and a non-empty bin,
   `mark = clamp(highlight_percentile, 0, 1) * total`. The bin that contains the mark (`above >= mark && below < mark`)
   sets `local_high[t] = t + (mark - below) / count`, a fractional bin index inside the bin. All other bins set `-1`.
   The tree reduction combines `local_high` with `max`, so the single marked bin wins. `total` and `mark` do not depend
   on `clip_low` or `clip_high`.
4. Thread 0 turns the sums into the target. It runs only when `local_kept[0] > 0`. Otherwise the previous luminance
   stays, clamped to the sensitivity limits.

```
log_target = local_sum[0] / local_kept[0]
if (local_high[0] >= 0.0) {
    log_high   = MIN_LOG2 + local_high[0] * BIN_WIDTH
    log_target = clamp(log_high - highlight_ceiling, log_target, log_target + highlight_max_shift)
}
target = luminance_adapt(exp2(log_target), adaptation, adaptation_pivot)
```

The trimmed mean uses bin centres. `log_high` is the lower edge of the marked bin plus a fraction of the bin width.

The rule works on the trimmed-mean target only:

- It never lowers `log_target`. The lower bound of the clamp is the trimmed mean itself.
- It raises `log_target` to `log_high - highlight_ceiling`, and by no more than `highlight_max_shift` above the trimmed
  mean. A raised metered luminance means a lower exposure. A frame with a bright area therefore darkens the exposure
  only when the protected level is more than `highlight_ceiling` stops above the target.
- With a percentile of 0.99, the brightest 1 percent of the weighted frame is ignored. The level just below it is the
  one that is protected.
- It runs before `luminance_adapt`. The adaptation mapping, the sensitivity clamp and the response still apply to the
  changed target.
- It also runs on the first frame and on immediate frames, because it changes `log_target` before the immediate branch.
- A maximum shift of 0 makes the rule a no-op. A percentile of 0 skips it. The target is then the trimmed mean.

After the target, the shader applies these steps in order:

1. Response. The result is the target itself when `set_immediate` is set or the previous luminance is 0. Otherwise it is
   the exponential response when `exponential` is set, and the linear step `prev + (target - prev) * exposure_adjust`
   when it is not. The result is clamped to the minimum and maximum sensitivity.
2. Light limit, when `light_limit > 0` and `prev > 0`: `result = max(min(result, prev * light_limit), min_luminance)`.
3. Hold, when `hold` is set and `prev > 0`: `result = prev`. It applies also on a frame without kept weight.

#### Exponential response, light limit and hold

With `exponential` set, the meter moves toward the target in log space, and the rate depends on the direction:
`rate = target > prev ? light_adjust : dark_adjust` and
`result = clamp(prev * exp2(log2(target / prev) * rate), min, max)`. `RendererSceneRenderRD` fills `light_adjust` and
`dark_adjust` only when the exponential response is on. Each is `1 - exp(-time_step / max(time, 0.001))`, with the light
or dark adaptation time from the camera attributes (defaults 0.5 s and 2.0 s). The camera attribute setter
clamps both times to at least 0.001 s. Without the exponential response, both fields keep the default of 1.0, and the
shader does not read them.

`light_limit` is `AutoExposureGuard::light_factor(stops_per_second, time_step, set_immediate)` from
`servers/rendering/storage/auto_exposure_guard.h`. It is `2^min(stops_per_second * time_step, 100)`, and 0 when the
limit is not above 0, `time_step` is not above 0, or the frame is immediate. The result is capped at that factor times
the previous luminance, so the metered luminance cannot rise faster than the limit. `hold` is the camera attribute
`auto_exposure_hold`, forced off on immediate frames. `Luminance` also zeroes both fields when `p_set` is true. The
mean meter's reduce shaders apply the same two rules after their linear step, using the push constant words `light_limit`
and `hold`.

### Edge cases

- Cut or reset: `rb->get_history_reset()` becomes `reset_history`, so the temporal grid restarts from the current frame.
  The same flag makes auto exposure set its value at once.
- First frame: a new `Buffers` object has `history_valid = false` and `history_index = 0`. The first temporal step
  copies the current grid. The histogram meter takes the target directly when the previous luminance is 0.
- Resize: see "Buffers". The sizes are recomputed, the history is invalid and the grid starts again.
- No camera attributes, or auto exposure off: no histogram is run, so the highlight policy does nothing. Local exposure
  still works on top of the fixed exposure.
- Other renderers: Mobile and Compatibility keep the stored settings and never run the pass or the histogram policy.
  The Mobile renderer's mean meter does not read the percentile settings. Stereo viewports skip local exposure.
- Debug draw modes that disable effects: no pass runs, and the tonemapper uses its path without local exposure.
- A frame with no luminance weight: the median is `1e30` and the tonemapper uses the fixed pivot. A grid lookup with a
  weight of 1e-4 or less uses the pixel's own luminance. `wide` uses the log2 of the floor.
- Non-finite pixels: the pass clamps luminance to 2^-20 to 65504 before the log, so NaN and infinity cannot enter the
  grid. In the histogram meter, NaN samples are skipped, positive infinity goes to the last bin, and zero, negative or
  negative-infinite luminance goes to the first bin.
- A paused frame (`time_step = 0`): with temporal smoothing on, a valid history and no reset, the history carries over
  unchanged. With an invalid history or a reset, the current grid is copied.
- Pass failure: `process` returns an empty `RID`, the tonemapper runs without local exposure for that frame, and the
  buffers stay allocated. `LocalExposure` is freed in the `RendererSceneRenderRD` destructor.

### Files

- `servers/rendering/renderer_rd/effects/local_exposure.h`, `local_exposure.cpp`: the pass, buffers and settings.
- `servers/rendering/renderer_rd/shaders/effects/local_exposure.glsl`: the six compute modes.
- `servers/rendering/renderer_rd/effects/tone_mapper.h`, `tone_mapper.cpp`, `shaders/effects/tonemap.glsl`: gain.
- `servers/rendering/renderer_rd/effects/luminance.h`, `luminance.cpp`, `shaders/effects/luminance_histogram.glsl`:
  histogram meter and `HistogramPolicy`. `luminance_reduce.glsl`, `luminance_reduce_raster.glsl` and
  `luminance_reduce_raster_inc.glsl` hold the mean meter.
- `servers/rendering/renderer_rd/renderer_scene_render_rd.cpp`, `.h`: frame order, settings and wiring.
- `servers/rendering/storage/local_exposure_limits.h`: limits and the reference median.
- `servers/rendering/storage/auto_exposure_guard.h`: light limit and hold helpers.
- `servers/rendering/storage/environment_storage.cpp`, `camera_attributes_storage.cpp`: stored values and clamps.
- `scene/resources/environment.cpp`, `camera_attributes.cpp` and the `RenderingServer` bindings in
  `servers/rendering/rendering_server.cpp`: the properties and the script entry points `environment_set_local_exposure`
  and `camera_attributes_set_auto_exposure_highlight`.
- `doc/classes/Environment.xml`, `CameraAttributes.xml`, `ProjectSettings.xml`, `RenderingServer.xml`: the class
  reference.
- `tests/servers/rendering/test_local_exposure_limits.cpp`, `test_auto_exposure_guard.cpp`: unit tests for the limits,
  the median reference and the guard helpers.
