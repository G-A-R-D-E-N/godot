# Godot code notes

### `modules/gi_restir_di/shaders/di_resample.glsl:di_emitter_pick`

The CDF search selects the first cumulative weight strictly above the random target, so zero-weight plateaus cannot select excluded emitters. The target is kept below the finite final CDF value even when floating-point multiplication rounds upward. Fresh and held samples use that same final value for their normalization; parallel scan reduction can round differently.

### `servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::update`

Emitter-table invalidation includes the instance filter and proxy status. The sampling generation also changes when proxy eligibility or a textured material's fallback mean luminance changes, so temporal reuse cannot keep a proposal drawn from the previous distribution.
