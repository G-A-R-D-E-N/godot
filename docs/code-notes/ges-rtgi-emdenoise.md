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

## Emitter radiance units (ray_scene.cpp _store_material, di_resample.glsl emitter reads)
Masked emission without texture ranges: the raster OpenPBR path computes
`emission_color x emission_luminance x tex.rgb x tex.a x openpbr_emission_scale`; the ray
path used `tex.rgb x tex.a x openpbr_emission_scale`, dropping the nits while the pick pdf
used the material's full nits, so textured emitters came out short by about
1/emission_luminance. `emission_texel_scale` now carries `emission_luminance x bounce x
openpbr_emission_scale` for those materials. The emission_color tint, oc_emission_scale and
flicker are still not on the ray path (no room in the material struct).
The emitter term is now multiplied by `ray_scene_emissive_exposure_normalization()` at both
emitter reads, as lamps (light_energy) and raster emission already are.

## Second emitter shadow ray (direct_shadows.glsl read_entries / emitter factor, direct_shadows.cpp params.pad[0])
`params.pad[0]` is a bitfield: bit 0 = emitter samples present, bit 1 = the resampler already traced emitter visibility (`emitters/visibility`). With bit 1 set, direct_shadows no longer traces its own per-pixel 0/1 ray to the emitter sample and writes factor 1: that ray re-grained the spatially filtered emitter term pixel by pixel and counted visibility twice.

## EMITTER_NEAR debug listing (ray_scene.cpp _update_triangle_emitters)
Off unless `rendering/global_illumination/ray_scene/debug_emitters_near` (Vector3, world) is set; `debug_emitters_radius` defaults to 8 m. Prints every emissive instance within the radius with its mesh path and, per emissive surface, the stored emission, mean luminance, atlas layer, texel scale and nits range, so the emitter lighting a given spot can be named.

## Skipped materials are not emitters (ray_scene.cpp _update_triangle_emitters)
Materials flagged MATERIAL_FLAG_SKIP (no shadow casting, or additive blending with skip_additive_surfaces on) are invisible to ray hits and occlusion, so DDGI never sees them. They were still entered into the resampler's triangle emitter list, so additive glow cards lit the room through the direct emitter term only. They are now left out, so both paths see the same scene. EMITTER_NEAR prints a skip column.

EMITTER_NEAR also prints, per emissive surface, its triangle count, world area and flux pi x area x mean_lum (lm), the same formula DerivedLightPolicy.Flux uses for the proxy lamp (whose lumens are then doubled by AreaLightLumens).

LAMP_NEAR (ray_scene.cpp _pack_light): with debug_emitters_near set, each packed lamp within the radius is printed once with type, world position, intensity (lm), energy, indirect energy, linear colour, range and cull mask (0 = stood down), so traced surface flux can be compared with the authored lamp that stands for the same fixture.

## Per-instance emitter colour and scale (ray_scene.cpp _instance_slots / instance assembly; MaterialStorage vec4 instance read)
The ray scene now honours the OpenPBR instance uniform `oc_emission_color` (vec4, alpha > 0 means placed) the way the raster OPBR_PLACED_EMISSION does: for an untextured emissive surface the stored emission becomes `placed.rgb x nits`, with nits = mean_luminance / luminance(linear emission_color). `oc_emission_scale` now also scales `emission_texel_scale`, so textured emitters follow it too. The game uses both to give a fixture traced in place of an authored lamp that lamp's chromaticity and lumens (MC decision 26 Nov 08:30).
