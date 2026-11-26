# ges-rtgi-emdenoise: emitter term temporal mean and spatial filter

## modules/gi_restir_di/shaders/di_resample.glsl

### di_emitter_hit_fraction / di_emitter_hold / di_emitter_accumulate
The held emitter sample now carries a hit fraction in `surface.w`: the running mean
(same weight as the colour mean, mass capped at 20) of whether the fresh sample each
frame reached the light. A frame whose fresh sample is rejected (facing, area or
shadow) folds in a zero instead of re-emitting the held value at full strength. Before,
the mean was built only from samples that got through, which biased occluded pixels
bright and pinned one lucky sample to a pixel indefinitely (vault 111: `held_only`
2.7M of 3.7M pixels per frame, `mean_age` rising one per frame). The written colour
is `colour * hit`. A stored zero bit pattern reads as 1 (entries written before the
field existed); a pixel whose fraction falls below 1e-3 is released.

### MODE_EMITTER_SPATIAL main
A depth/normal-guided filter of the emitter contribution (colour x geometric), 12
taps minimum within `spatial_radius`. Zero-contribution neighbours on the same
surface are counted, so shadowed surfaces are not biased bright. The direction keeps
one neighbour's point drawn in proportion to contribution. Fixed on the way: the pass
returned for every pixel because it gated on the lamp-reservoir `tile_flags` stamp
(`spatial_pixels_rewritten 0`), and its depth test compared the non-linear depth
buffer against a tolerance in metres. A pixel the filter fills gets its emitter mark
set, otherwise the filter could only dim pixels.
