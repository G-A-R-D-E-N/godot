# Godot code notes

### `modules/gi_restir_di/shaders/di_resample.glsl:di_emitter_pick`

The CDF search selects the first cumulative weight strictly above the random target, so zero-weight plateaus cannot select excluded emitters. The target is kept below the finite final CDF value even when floating-point multiplication rounds upward. Fresh and held samples use that same final value for their normalization; parallel scan reduction can round differently.

### `servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::update`

Emitter-table invalidation includes the instance filter and proxy status. The sampling generation also changes when proxy eligibility or a textured material's fallback mean luminance changes, so temporal reuse cannot keep a proposal drawn from the previous distribution.

### `modules/gi_restir_di/shaders/di_resample.glsl:di_emitter_accumulate` and `MODE_EMITTER_SPATIAL`

Each proposal retains its own emitter point, color, and geometry. Temporal and spatial reuse select one complete proposal by its receiver-side importance and carry a normalized reservoir weight to the final shadow and lighting passes. A positive importance floor keeps zero-contribution proposals eligible when they may contribute at another receiver. The estimator still needs GPU validation against a scene with differently colored occluded and visible emitters.

`modules/gi_restir_di/shaders/di_resample.glsl:di_emitter_history_store` retains the sampled barycentrics so textured emission can be reevaluated without reconstructing UVs from a potentially thin triangle. `modules/gi_restir_di/direct_resampler.cpp:DirectResamplerRD::_ensure_emitter_samples` checks the expanded buffer size before narrowing to the rendering device's 32-bit size argument. `servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl:fragment` applies the reservoir weight in full precision before narrowing the lighting result.
