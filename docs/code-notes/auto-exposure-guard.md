# Auto exposure rise limit and hold

`CameraAttributes` has two properties that act on the auto exposure meter after it has computed its new value.
`auto_exposure_light_adaptation_limit` caps how fast the metered luminance may rise. `auto_exposure_hold` keeps the
metered luminance at its stored value. Both apply to the log histogram meter (`luminance_histogram.glsl`) and to the
mean meter (`luminance_reduce.glsl` on the compute path, `luminance_reduce_raster.glsl` on the raster path). With the
defaults, the output of every meter is unchanged.

The stored value is the divisor of exposure. `tonemap.glsl` computes
`exposure *= 1.0 / (stored * luminance_multiplier / auto_exposure_scale)`. A rise of the stored value therefore lowers
the exposure and darkens the image. The limit slows how fast the image darkens when the scene gets brighter. The hold
freezes the exposure. The tonemap of the same frame reads the value that the guarded pass has just written.

### Settings and defaults

| Property | Type | Default | Range hint |
|---|---|---|---|
| `auto_exposure_light_adaptation_limit` | float, stops per second | `0.0` (no limit) | `0,64,0.1,or_greater` |
| `auto_exposure_hold` | bool | `false` | |

- Both are members of `CameraAttributes`, so `CameraAttributesPractical` and `CameraAttributesPhysical` have them.
- The setters, getters and `ClassDB` bindings are in `scene/resources/camera_attributes.cpp`:
  `set_auto_exposure_light_adaptation_limit`, `get_auto_exposure_light_adaptation_limit`, `set_auto_exposure_hold`
  and `is_auto_exposure_hold`.
- `CameraAttributes::_validate_property` hides every `auto_exposure_*` property except `auto_exposure_enabled` in the
  editor while auto exposure is off. Both properties follow that rule.
- The class reference describes both properties in `doc/classes/CameraAttributes.xml` and the server call in
  `doc/classes/RenderingServer.xml`.

The limit is a rate in stops (powers of two) per second, so the same value gives the same movement per second at any
frame rate. It is measured on the stored value, which has already passed through `luminance_adapt`
(`luminance_adaptation_inc.glsl`). With `auto_exposure_adaptation` below `1.0`, a scene luminance under the adaptation
pivot is compressed as `pivot * pow(l / pivot, strength)`. One stop of the stored value there is more than one stop of
scene luminance. The rate is a rate of the adapted value, not of raw scene luminance.

### `servers/rendering/rendering_server.h`

`RenderingServer::camera_attributes_set_auto_exposure_guard(camera_attributes, light_adaptation_limit, hold)` is the
server-side entry point. It is declared in `rendering_server.h`, bound in `rendering_server.cpp` and forwarded in
`rendering_server_default.h` with `FUNC3`.

`RendererCameraAttributes::camera_attributes_set_auto_exposure_guard`
(`servers/rendering/storage/camera_attributes_storage.cpp`) stores the pair in the camera attributes record.

- It clamps the limit with `MAX(limit, 0.0f)`. A negative value therefore acts as `0.0`.
- `CameraAttributes::set_auto_exposure_light_adaptation_limit` stores the raw value and its getter returns it
  unclamped. The property can read back a negative value while the server uses `0.0`.
- The renderer reads the pair with `camera_attributes_get_auto_exposure_light_adaptation_limit` and
  `camera_attributes_get_auto_exposure_hold`.
- The setter and both getters report an error for an invalid RID. The getters return `0.0` and `false`.

### `scene/resources/camera_attributes.cpp`

- `CameraAttributes::_update_auto_exposure_response` sends three groups: the response settings
  (`camera_attributes_set_auto_exposure_response`), the guard pair and the highlight policy
  (`camera_attributes_set_auto_exposure_highlight`). It then emits `Resource.changed`.
- The setters for the exponential response, both adaptation times, the limit and the three histogram highlight
  properties call it. A change to any of them therefore sends the guard pair again.
- `CameraAttributes::set_auto_exposure_hold` calls `camera_attributes_set_auto_exposure_guard` directly and does not
  emit `Resource.changed`. This keeps a hold that is raised and lowered every few frames from notifying the editor
  and other listeners.
- Neither path touches `auto_exposure_version`. Only `camera_attributes_set_auto_exposure` bumps it, and only when auto
  exposure goes from off to on. Raising a hold or changing the limit therefore never resets the meter.

### `servers/rendering/storage/auto_exposure_guard.h`

The header holds three inline functions in `namespace AutoExposureGuard`.

- `light_factor(p_stops_per_second, p_time_step, p_off)` returns the largest allowed ratio of new to previous metered
  luminance for one frame:

  ```
  factor = 2 ^ min(stops_per_second * time_step, 100)
  ```

  It returns `0.0f`, meaning "no limit", when `p_off` is true, when `p_stops_per_second` is not greater than zero
  (this includes NaN), or when `p_time_step` is not greater than zero. The exponent is capped at 100 stops. A very
  long frame or a very large limit then gives a finite float (about 1.27e30), never an infinity.
- `limited(p_previous, p_stepped, p_factor, p_min_luminance)` returns
  `MAX(MIN(p_stepped, p_previous * p_factor), p_min_luminance)` when `p_factor > 0` and `p_previous > 0`. Otherwise it
  returns `p_stepped`.
- `held(p_previous, p_stepped, p_hold)` returns `p_previous` when `p_hold` is true and `p_previous > 0`. Otherwise it
  returns `p_stepped`.

The renderer calls only `light_factor`. `limited` and `held` mirror the formulas in the shaders and are used by the
unit tests (`tests/servers/rendering/test_auto_exposure_guard.cpp`).

Example: a limit of `4.0` at 60 Hz gives `2^(4/60)`, about 1.0473 per frame. A target eight times higher than the
stored value is reached no faster than 4 stops per second. Three frames of a flash move the meter by at most 0.2
stops.

### `servers/rendering/renderer_rd/renderer_scene_render_rd.cpp`

The guard is computed once per frame in `RendererSceneRenderRD::_render_buffers_post_process_and_tonemap`, inside the
"Auto exposure" block. The block runs only when both conditions hold.

- `can_use_effects` is true. It requires a target size of at least 8x8 and a debug draw mode that allows effects
  (`_debug_draw_can_use_effects`).
- `camera_attributes_uses_auto_exposure` is true for the camera attributes of the render.

Without camera attributes, with auto exposure off, with a smaller target or with a debug draw mode that disables
effects, the block is skipped and neither property has any effect.

In order:

1. `set_immediate` is true when the camera attributes `auto_exposure_version` differs from the one stored in the render
   buffers, or when `RenderSceneBuffersRD::get_history_reset` is true. The history reset is true on a history cut (a
   change of `rendering/anti_aliasing/quality/history_reset_serial`) or when the exposure normalization changed.
2. `auto_exposure_hold = !set_immediate && camera_attributes_get_auto_exposure_hold(...)`.
3. `auto_exposure_light_limit = AutoExposureGuard::light_factor(limit, time_step, set_immediate)`. `time_step` is the
   raw frame step given to `RendererSceneRenderRD::set_time`. It does not depend on
   `rendering/environment/auto_exposure/clamp_adjust`, which clamps only the `step` of the exposure blend, nor on
   `auto_exposure_speed`.
4. The pair goes to the meter that runs.
   - Log histogram meter: `histogram_policy.light_limit` and `histogram_policy.hold` in `LuminanceHistogramPolicy`,
     passed as the trailing `p_policy` argument of `Luminance::luminance_histogram_meter`. The meter runs only when
     `Luminance::supports_histogram_meter` is true, which needs the compute effects path.
   - Mean meter, and the log histogram setting on the raster path (the code prints a warning once and uses the mean
     meter): the last two parameters of `Luminance::luminance_reduction`, `p_light_limit` and `p_hold`.
5. With `rendering/environment/auto_exposure/debug_log` on, the `AEX_LOG` line prints `light_limit` and `hold`. The
   printed `light_limit` is the factor from step 3, not the stops-per-second setting. It is `0` both when no limit is
   set and on a reset frame, and the `immediate` field tells the two apart.

The histogram policy also carries the highlight settings and the exponential response. `light_adjust` and
`dark_adjust` are computed only when the exponential response is on, as `1 - exp(-time_step / max(time, 0.001))` for
the light and dark adaptation times.

Every viewport keeps its own stored value (in `LuminanceBuffers`, one per `RenderSceneBuffersRD`) and its own
`set_immediate`. Viewports that share one `CameraAttributes` resource share the limit and the hold. `time_step` is a
renderer-wide value.

### `servers/rendering/renderer_rd/effects/luminance.h` and `luminance.cpp`

The structs and the default arguments are declared in `luminance.h`. `luminance.cpp` fills them and applies the
`p_set` masking. The push constant structs and the GLSL blocks that read them are:

- `LuminanceReducePushConstant` (`luminance_reduce.glsl`) and `LuminanceReduceRasterPushConstant`
  (`luminance_reduce_raster_inc.glsl`, shared by the vertex and fragment stages of `luminance_reduce_raster.glsl`).
  Both end in `float light_limit; uint32_t hold;`.
- `LuminanceHistogramPushConstant` (`luminance_histogram.glsl`). It ends in `float highlight_percentile;
  float highlight_ceiling; float highlight_max_shift; float light_adjust; float dark_adjust; uint32_t exponential;
  float light_limit; uint32_t hold;`. The block is larger than the two reduce blocks. The values come from the
  `const HistogramPolicy &p_policy = HistogramPolicy()` parameter of `Luminance::luminance_histogram_meter`.

`LuminanceHistogramPolicy` (alias `Luminance::HistogramPolicy`) defaults to `light_limit = 0.0` and `hold = false`. A
caller that passes no policy gets no guard. `Luminance::luminance_reduction` takes `p_light_limit = 0.0` and
`p_hold = false` by default, with the same result.

Both functions write

```
light_limit = p_set ? 0.0 : limit factor
hold        = (hold && !p_set) ? 1 : 0
```

so a reset frame is guarded a second time here, independent of the renderer. For the mean meters this has no effect on
the result, because a reset frame does not run the guarded last stage at all (see below). It matters for
`luminance_histogram.glsl`, whose resolve dispatch runs on every frame.

### Shader behaviour

The previous metered value is `prev`. It is read from the one-pixel `prev_luminance` texture
(`LuminanceBuffers::current`). The last pass of each meter writes the new value into the last `reduce` texture, and
`Luminance` then swaps that texture with `LuminanceBuffers::current`. Earlier reduction levels write partial sums and
the histogram accumulate pass writes bins. The guard is not part of them.

Order in the last pass:

1. The normal step toward the target, with the target already passed through `luminance_adapt`
   (`auto_exposure_adaptation`). This is a linear blend by `exposure_adjust`, or for the histogram meter with the
   exponential response on, `prev * exp2(log2(target / prev) * rate)` with `light_adjust` or `dark_adjust` as `rate`.
2. The rise limit, when `light_limit > 0.0` and `prev > 0.0`.
3. The hold, when `hold != 0` and `prev > 0.0`.

The limit and the blend act in series on a rise, so the slower of the two wins. The limit acts on the value that sets
the exposure.

Clamping to `min_luminance` and `max_luminance` differs per shader.

- `luminance_histogram.glsl` (`RESOLVE`, thread 0) and `luminance_reduce.glsl` (`WRITE_LUMINANCE`) clamp the blended
  value. The limit is `result = max(min(result, prev * light_limit), min_luminance)`. The floor keeps the limit from
  taking the meter under `min_luminance` when `prev` itself lies under it, for example after the minimum was raised.
  In that case the meter moves to `min_luminance` in one frame, and the limit does not slow that step.
- `luminance_reduce_raster.glsl` (`FINAL_PASS`) clamps the target before the blend, and the blended value is not
  clamped again. The limit is `luminance = min(luminance, prev_lum * light_limit)`. There is no floor term. When
  `prev_lum` is under `min_luminance`, the raster meter rises from `prev_lum` by at most the factor per frame.

`light_limit` is the factor `2^(stops_per_second * time_step)`. A value that is already lower than `prev * light_limit`
is untouched. A darker scene is never limited, and a rise inside the allowance passes through.

Histogram resolve:

- The limit sits inside the `local_kept[0] > 0.0` branch, after both the linear and the exponential blend. When no
  pixel weight is kept, the limit is skipped and the result is `clamp(prev, min_luminance, max_luminance)`.
- The hold applies after that branch. When no weight is kept, a raised hold replaces the clamped `prev` with the
  unclamped `prev`.
- The highlight rule of the histogram meter (`auto_exposure_highlight_percentile`, `_ceiling`, `_max_shift`) is
  computed from the current frame's histogram (`local_high`) and does not read `prev`.

Hold, in every variant: the hold replaces the result with `prev` and comes last, so it wins over the limit. It does not
clamp `prev` again. If `auto_exposure_min_sensitivity` or `auto_exposure_max_sensitivity` change during a hold, or
`prev` was outside them, the held value stays outside the new range until the hold is lowered.

#### When the guarded pass runs

- Compute mean meter. `Luminance::luminance_reduction` binds the `WRITE_LUMINANCE` variant only for the last level of a
  chain of at least two levels, and only when `p_set` is false. A chain has one level when the internal size is 8x8 or
  smaller. That level runs `READ_TEXTURE`, which writes the plain adapted average with no blend, no limit and no hold.
  In that case both properties are ignored.
- Raster mean meter. The last pass uses the `FINAL_PASS` variant when `p_set` is false and the destination is 1x1,
  including a chain of one level. Both properties apply there.
- Reset frame, mean meters. The last pass is the plain reduce stage (`luminance_reduce.glsl` without
  `WRITE_LUMINANCE`, or a raster pass without `FINAL_PASS`). It has no `prev`, no blend, no limit and no hold, and it
  does not clamp to `min_luminance` and `max_luminance`. The histogram meter clamps on a reset frame.

### Mean meter highlight limit and the hold

The mean meter's `auto_exposure_highlight_limit` caps each sampled pixel at the unadapted previous value times
`2^limit`. The previous value is read in `luminance_reduce.glsl` (`READ_TEXTURE`) and in `luminance_reduce_raster.glsl`
(`FIRST_PASS`). During a hold, `prev` is the held value, so the cap works from it. The histogram meter does not use
`auto_exposure_highlight_limit`.

### What a hold does to history and the first frame after it

- The passes still run during a hold. The reduction and the histogram are computed and discarded, and the texture swap
  still happens. The stored value after a held frame equals the value before it exactly.
- The meter keeps no memory of the target while held. Nothing accumulates and no catch-up happens. On the first frame
  after the hold is lowered, the meter takes one normal step (and one limited step, if a limit is set) from the held
  value toward the current target. If the scene changed during the hold, the exposure moves at its usual speed, not in
  one jump.
- A hold is ignored when the stored value is zero. `LuminanceBuffers::current` is cleared to zero when it is created.
  On such a frame the meter produces its value as usual.

### Edges

- Reset or cut: when `set_immediate` is true (first use of the camera attributes by a render buffer set, auto exposure
  turned on, history cut, exposure normalization change), the limit factor is `0.0` and the hold is off. The meter
  sets its value from the target at once. A hold that is still raised applies again from the next frame, using the
  freshly set value.
- First frame: `prev` is zero, so neither the limit nor the hold applies. Every guard in the shaders tests
  `prev > 0.0`.
- Resize: `LuminanceBuffers::configure` rebuilds the reduction chain. It keeps `current` unless its usage bits or the
  luminance multiplier changed. A kept `current` keeps its value, so a limit or a hold continues across a resize. A
  recreated one starts at zero and behaves like a first frame.
- Frame step: a frame with a zero or negative `time_step` has no limit. A long frame has a larger allowance, up to the
  100-stop cap.
- Tiny products: when `stops_per_second * time_step` is so small that `factor` rounds to exactly `1.0f`, the meter
  cannot rise while the limit is set, except through the `min_luminance` floor of the compute and histogram shaders.
- Forward+: the Forward+ renderer runs the block through the compute path, with either meter.
- Mobile: `RenderForwardMobile::_render_buffers_can_be_storage` returns false, so the raster mean meter is the one
  that runs, and a log histogram setting falls back to it with a one-time warning. The block runs only when the post
  process is not done in the subpass. `RenderForwardMobile` turns the subpass off when the environment is valid and
  auto exposure is used. The subpass path (`RendererSceneRenderRD::_post_process_subpass`) rejects auto exposure with
  an error, and the properties do nothing there. The class reference documents auto exposure as supported in Forward+
  only.
- Compatibility: the renderer has no luminance code. The properties are stored and not used.

## Highlight shift time

`CameraAttributes.auto_exposure_highlight_adaptation_time` and `auto_exposure_highlight_release_time` (seconds, 0 to 60,
default 0 each) give the log histogram meter's highlight shift its own state.

- Shift: `raw = clamp(log_high - highlight_ceiling - log_mean, 0, highlight_max_shift)` in stops, the same amount the
  rule used to add in one step, or 0 when the rule finds no mark. The target is `log_mean + shift`. Adaptation, the
  response and the rise limit then act on it unchanged.
- Update: a larger `raw` is approached at `highlight_rise = 1 - exp(-time_step / adaptation_time)` per frame, a smaller
  one at `highlight_fall` from the release time. A time of 0 or a reset gives a rate of 1, which takes `raw` at once. A
  frame with no time step (time scale 0) gives a rate of 0 for a time above 0, so a paused game neither deepens nor
  releases the shift where that time is set; a direction whose time is 0 still follows `raw` at once.
- Bound: a stored shift is cut to the current `highlight_max_shift` before the update, so lowering the cap takes
  effect on the next frame. A stored shift that is not a number or below 0 is replaced by `raw`.
- Why a slow rise and a short release: pan tests (day-turn-sanctuary) showed a symmetric slow filter still pumps, because
  the shift rises and falls inside a turn and the lag adds its own swing. A slow rise keeps a passing sky patch from
  building a shift. An instant release turned the shift into a lower envelope: a flickering source that drops under
  the ceiling for one frame in twenty kept it near 0 (a review model: mean shift 0.07 of 2 stops). A release of a few
  tenths of a second makes a one-frame dip cost about 5 % of the shift and still leaves no slow tail.
- Exactness: the shader keeps the old expression `clamp(log_high - ceiling, log_mean, log_mean + max_shift)` as
  `stepped` and uses it whenever the shift in use equals `raw` (both times 0, a reset, or a shift that has caught up).
  Only a shift still moving toward `raw` uses `log_mean + shift`. `log_mean + raw` alone differs from the old value by
  an ulp on some frames (0.1 % to 10 % in two float32 models), so both times at 0 would not have been the old rule bit
  for bit. One doctest runs 4000 random cases with both rates at 1 against the old expression and asks for equality.
- State: two floats after the 256 bins in the histogram storage buffer, `shift` and `shift_valid`. The per-frame
  `buffer_clear` covers only the bins. The stored shift is used only when `shift_valid` is set, the previous luminance
  is above 0, the frame is not `set_immediate`, and the mean meter has not run since the last histogram frame
  (`LuminanceBuffers::histogram_shift_stale`, set by `luminance_reduction`, passed as `shift_stale` and cleared by the
  histogram meter). Otherwise the frame takes `raw`. The buffer is created zeroed, so a new or resized view starts
  invalid. A hold keeps the stored shift. A frame with no kept samples keeps the shift and its valid flag as they were.
- CPU mirror for the tests: `AutoExposureGuard::highlight_rate`, `highlight_valid` (the four conditions above),
  `highlight_stored` (the valid flag written back), `highlight_shift` and `highlight_step`.
- Push constant: `highlight_rise`, `highlight_fall`, `shift_stale` and one pad word, 96 bytes in all.
- A camera cut the game does not flag as a reset keeps the shift. Release is never slower than the release time, so
  the darker side is never stuck; the brighter side may take up to the adaptation time to deepen after such a cut.
- The mean meter and the raster path have no highlight rule and do not read these settings.
