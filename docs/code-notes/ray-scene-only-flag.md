# Ray-scene-only instance flag

`servers/rendering/rendering_server_enums.h:InstanceFlags` adds `INSTANCE_FLAG_RAY_SCENE_ONLY` (value 4, default
off). It keeps a mesh instance out of the camera draw lists while the global illumination ray scene still traces it.

- `servers/rendering/renderer_scene_cull.h:InstanceData`: `FLAG_RAY_SCENE_ONLY = 1 << 25`; `Instance::ray_scene_only`
  (default false).
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::instance_geometry_set_flag`: the new case refuses to
  turn the flag on for a base that is not `INSTANCE_MESH`, because only mesh geometry enters the ray scene; multimesh,
  particles and the other geometry bases are not admitted, so a flagged one would vanish. The base must be set first.
  It sets the flag on the instance, and on its InstanceData when the instance is in a scenario.
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::_update_instance`: when the instance's InstanceData is
  built, the flag is copied from `ray_scene_only` only when the base is `INSTANCE_MESH`, so an instance whose base
  later changes to another type is drawn normally.
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::_scene_cull`: the camera draw branch (`LAYER_CHECK`,
  frustum, visibility, occlusion and `FLAG_IGNORE_ALL_CULLING`) is skipped for flagged instances. The ray scene admit
  branch does not test the flag, so the instance is still collected into `ray_scene_geometry_instances` when it is
  inside the ray scene region and passes the layer and visibility checks.
- Shadows are separate. Neither the directional cascade cull in `RendererSceneCull::_scene_cull` nor the omni and spot
  culls in `RendererSceneCull::_light_instance_update_shadow` test the flag, so a flagged instance still casts raster
  shadows unless its shadow casting is set off.
- Why a flag and not a layer: the ray scene admit and the camera draw branch of `RendererSceneCull::_scene_cull` both
  test `LAYER_CHECK` against the same camera `visible_layers`, so no layer assignment can hide an instance from the
  camera and keep it in the ray scene.
- `doc/classes/RenderingServer.xml:INSTANCE_FLAG_RAY_SCENE_ONLY`: enum constant entry. With the two flags below,
  `INSTANCE_FLAG_MAX` is 8 (the no-raster-shadow flag takes 7).
- `servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::shadow_cast_flags`: with shadow casting off, the
  ray scene tags every surface of the instance `MATERIAL_FLAG_SHADOW_PASS`, so shadow rays pass it. A flagged instance
  with casting off is therefore neither drawn nor a shadow caster, while bounce rays still stop on it. A caller that
  hides meshes with this flag keeps their ray-traced shadows unchanged only while their casting stays as it was.

## Ray scene exclude flag

`servers/rendering/rendering_server_enums.h:InstanceFlags` adds `INSTANCE_FLAG_RAY_SCENE_EXCLUDE` (value 6, default
off). It keeps a drawn mesh out of the ray scene, so it neither blocks nor bounces global illumination rays. A typical
use is a coarse stand-in mesh, such as an HLOD tile, that should not be traced next to the full-detail meshes it
stands for.

- `servers/rendering/renderer_scene_cull.h:InstanceData`: `FLAG_RAY_SCENE_EXCLUDE = 1 << 26`;
  `Instance::ray_scene_exclude` (default false).
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::instance_geometry_set_flag`: mesh instances only, set
  on the instance, and on its InstanceData when it is in a scenario. `RendererSceneCull::_update_instance` copies it
  only for a mesh base, as for the ray-scene-only flag.
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::_scene_cull`: tested in the outer condition of the ray
  scene admit branch, with the region-has-volume and layer checks; the region and visibility checks follow per base
  type (a mesh against `ray_scene_region`, a light against `ray_scene_light_region`). The camera draw branch and the
  shadow culls do not test it.
  With the ray-scene-only flag also set, the instance is in neither list: exclude wins.
- No unit test reaches `RendererSceneCull::_scene_cull`, so the admit test has no unit test.

## Ray scene occluder flag

`servers/rendering/rendering_server_enums.h:InstanceFlags` adds `INSTANCE_FLAG_RAY_SCENE_OCCLUDER` (value 5, default
off), the opt-in for the opposite: a mesh with shadow casting off that still blocks shadow rays. One use is a ray-only
stand-in for ground (`INSTANCE_FLAG_RAY_SCENE_ONLY`, casting off and this flag). On a drawn mesh it means a ray-traced
shadow without a raster shadow.

- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::instance_geometry_set_flag`: mesh instances only, like
  the ray-scene-only flag, so the base must be set first: turning it on for a non-mesh base is refused and nothing is
  stored. It sets `Instance::ray_scene_occluder` and pushes it to the geometry instance.
  `RendererSceneCull::instance_set_base` pushes the stored value again, which keeps it across a later mesh-to-mesh
  base change.
- `servers/rendering/renderer_geometry_instance.h:RenderGeometryInstanceBase::Data::ray_scene_occluder`: read by the
  ray scene, which only sees the geometry instance. There is no InstanceData bit, because the cull does not test it.
- `servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::shadow_cast_flags`: casting off with the flag
  gives 0 (every ray stops), the same as casting on. On, double sided and shadows only are unchanged by it. A material
  with `gi_shadow_pass` keeps `MATERIAL_FLAG_SHADOW_PASS`; the flag does not override it.
- `servers/rendering/renderer_rd/environment/ray_scene_backend.cpp:RayScene::instance_class`: the class follows the
  final flags, so an occluder with casting off and no candidate material is opaque to hardware traversal.
- `tests/servers/rendering/test_ray_scene_cast.cpp`, `test_ray_scene_class.cpp`: known answers with the flag, a quiet
  mesh (casting off), a cast-off occluder and a shadows-only occluder as named rows, skip still winning, and the
  surface candidate table with both flag values. Casting off with the flag gives 0, which the split-instance table
  already covers.

## Lights of every bake mode

`servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::_pack_light` packs a light whatever its bake mode.
The test that dropped `LIGHT_BAKE_DISABLED` lights is gone, so every light that lights the scene also lights the ray
scene's hit points and gets its ray-traced shadow. Zero-size area lights and lights past `p_limit` are still dropped.

- The packed list is the one light list of the ray scene, so bake-disabled lights now reach every reader of it: the
  DDGI, surfel and `TRACE_MODE_RTGI` shading (`shade_hit`), the ReSTIR GI module's shading (`gi_hit_direct_light`,
  `rs_fresh_lo`, `rs_replay_shade`) and `DDGILightTracker::observe`, which `_pack_light` calls for each packed light.
  Their changes therefore fire light events, which reach ReSTIR GI's event re-shade and history caps like those of
  any other packed light. Light ids (`Light::pad`) do not change.
- They also take slots like every other light: directional lights under the `max_lights` setting, local lights as
  candidates for the cut described in `ray-scene-light-pick.md`.
- Not aligned: `servers/rendering/renderer_rd/environment/rtgi.cpp`, the directional samples for `RtgiRestarts`,
  still skips bake-disabled directional lights. A bake-disabled sun now shades GI hit points, but a jump of its
  direction starts no RTGI restart. The raster's own baked GI is unchanged.
- No unit test covers the bake mode: `_pack_light` needs a `LightStorage`, which the ray scene unit tests do not
  build.
