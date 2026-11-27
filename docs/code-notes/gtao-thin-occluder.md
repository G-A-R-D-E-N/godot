# GTAO thin occluder compensation

### `servers/rendering/renderer_rd/shaders/effects/xegtao_inc.glsl`

GTAO works in slices. For each pixel it picks a few screen-space directions, and along each one it steps through the depth buffer on both sides of the pixel. On each side it looks for the horizon: the highest angle, measured from the surface, at which a depth sample blocks the view. A high horizon means little open sky and strong occlusion.

The depth buffer holds only the nearest surface. A thin object in front of a wall, such as a grass blade, a wire or a railing, is therefore read as if it filled the whole gap behind it, and the wall is occluded as if by a slab.

XeGTAO has a term for this, `ThinOccluderCompensation`, and this file implements it. In `XeGTAO_MainPass`, inside the loop over steps (which is inside the loop over slices), each sample's offset from the shaded point is taken in view space. `falloffBase0` and `falloffBase1` are the length of that offset with its view-depth component multiplied by `1.0 + thinOccluderCompensation`. That length sets the sample's weight: 1 near the point, falling to 0 at the effect radius. A sample with a lower weight moves the horizon less.

So a sample whose view depth differs from the shaded point's, in front or behind, counts as farther away than it is and occludes less. A sample at the same view depth keeps its weight. The direction to the sample is not changed, only its weight.

The term is a heuristic. It cannot tell a thin object from a thick one; it only discounts a difference in view depth. A higher value also lightens the occlusion a thick object casts on a surface some way behind it, and the occlusion in a crease seen at a shallow angle, where neighbouring samples differ in view depth.

It adds no samples. The only extra work is one multiply per sample.

### `servers/rendering/renderer_rd/effects/ss_effects.cpp`

`SSEffects::generate_gtao` reads the project setting `rendering/environment/gtao/thin_occluder_compensation` with `GLOBAL_GET_CACHED`, clamps it to `0.0`..`0.7`, and stores it in `GTAOPushConstant::thin_occluder_compensation`, the constant block passed to the shader. The cached read refreshes when any project setting changes, so a value set at run time takes effect on the next call.

The editor range does not enforce a limit on values set from script or from a project file, so the clamp in this function is what keeps a finite value inside the range. The shader accepts any value; the `0.7` limit is set here.

It is one value for every viewport that runs GTAO. It is a project setting, not an `Environment` property. The other ambient occlusion backend, `SSEffects::generate_ssao`, does not read it.

### `servers/rendering/rendering_server.cpp`

`RenderingServer::init` defines the setting with the other `rendering/environment/gtao/` settings: a float, default `0.0`, editor range `0.0` to `0.7` in steps of `0.01`. At `0.0` the shader's weight is the plain distance falloff.
