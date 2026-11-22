# Ray-scene shadow rays: the near back-face skip

### `servers/rendering/renderer_rd/shaders/environment/ray_scene_inc.glsl:ray_scene_unshadowed`

Shadow rays in the ray scene start at `p + n * bias`, where `n` is the receiver normal. In the direct shadow pass, `n`
comes from the 8-bit normal-roughness buffer. The quantization tilts it by about 0.002 to 0.005 rad, so the origin moves
sideways by bias times that tilt: about 4e-5 to 1e-4 m at a 0.02 m bias. Where a receiver touches another closed mesh
at a concave crease, that push can put the origin a few 1e-5 m inside the neighbour. Triangles are two-sided and there
is no tmin, so the ray hits the inside of the neighbour at once and every lamp reads blocked. The result is a one-pixel
dark line along the crease.

Rule: a shadow ray ignores a back-face hit closer than `0.25 * bias` on a material that is neither
`RAY_SCENE_MATERIAL_TWO_SIDED` nor `RAY_SCENE_MATERIAL_BLEND`. Front faces, two-sided and blend surfaces, and back
faces at or beyond the window block as before.

- The window scales with bias, as the origin error does. On a box model of the crease cases, windows of 5 mm and 1 mm
  both restore the lamps the true surface sees, while 0.2 mm leaves some crease pixels dark. 0.25 times a 0.02 m bias is
  5 mm.
- `ray_scene_unshadowed` passes `0.25 * p_bias`. It serves the direct shadow pass and `ray_scene_direct_light`, so
  shadow rays from DDGI and surfel hits use the window too (bias there is `0.01 + 0.001 * t`).
- Back face means the same thing as `RaySceneHit::backface`: `det > 0.0` in the software triangle test, and
  `!rayQueryGetIntersectionFrontFaceEXT` on hardware. Both are object-space facing.

### `servers/rendering/renderer_rd/shaders/environment/ray_scene_inc.glsl:ray_scene_trace_masked_near`

- It carries the window as `p_near_backface`. `ray_scene_trace_masked` wraps it with 0.0, so every other ray keeps its
  old result bit for bit.
- `ray_scene_trace_blas` tests `t < p_near_backface`, `det > 0.0` and the two flags right after the skip mask, before
  the alpha and band tests. `t` is the world ray parameter, because the object-space direction is not renormalized.

### `servers/rendering/renderer_rd/shaders/environment/ray_scene_inc.glsl:RAY_SCENE_SKIP_MASK`

- `ray_scene_trace_blas`, both `ray_scene_trace_masked_near`, `ray_scene_trace_masked` and `ray_scene_trace` take
  `p_shadow` after `p_any`. `p_any` means first hit only: it ends the leaf loop and the TLAS walk in software and sets
  `gl_RayFlagsTerminateOnFirstHitEXT` on hardware. `p_shadow` means a ray toward a light: it alone picks the skip,
  `RAY_SCENE_MATERIAL_SHADOW_PASS` when true and `RAY_SCENE_MATERIAL_SHADOW_ONLY` when false.
- `ray_scene_unshadowed` passes true, true; `ray_scene_trace_opaque` passes false, false; the other two forward both.
- Every other caller passes `p_shadow` equal to its `p_any`: the DDGI and ReSTIR GI rays false, false, and ReSTIR GI's
  `rs_visible` true, true, so every ray keeps its old skip. Giving `rs_visible` false, so it skips what the ReSTIR GI
  bounce rays skip, is ReSTIR GI's own change with its own on/off pair.
- No caller passes `p_any` false with `p_shadow` true.

### `servers/rendering/renderer_rd/shaders/environment/ray_scene_hw_inc.glsl:ray_scene_trace_masked_near`

- Two queries in one loop. The near pass covers `[0, min(near, t_max)]` with `gl_RayFlagsNoOpaqueEXT`, so opaque
  instances also reach the candidate loop and the skip can apply. If it commits a hit, the loop stops. Otherwise the
  main pass covers `[near, t_max]` with the usual flags.
- A back face at exactly `near` is skipped by the near pass and found by the main pass, which matches the software
  `t < near`. With a window of 0 only the main pass runs, from 0, as before.
- Only `ray_scene_unshadowed` uses a nonzero window, always with `p_any` and `p_shadow` true. A closest-hit caller
  would also work, because the near pass covers the closest hits and the blend tie query that follows only looks at
  blend surfaces.

Known limits:

- A closed partition thinner than the window can still let light through at a crease: an origin inside it leaves
  through the far face, which is also a back face. The reach is 5 mm at a 0.02 m bias.
- Mirrored instances (negative determinant) invert object-space facing on both backends, so the skip applies to
  their outward faces. `shade_hit` and the DDGI back-face count have the same behaviour.
- A two-sided or blend closed neighbour still blacks out a crease, because those surfaces are never skipped.
- On hardware each shadow ray runs a second, short ray query.
