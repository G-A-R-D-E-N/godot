# No-raster-shadow instance flag

`servers/rendering/rendering_server_enums.h:InstanceFlags` adds `INSTANCE_FLAG_NO_RASTER_SHADOW` (value 5, default
off; `INSTANCE_FLAG_MAX` moves to 6). It keeps an instance out of every light's raster shadow map while camera
drawing and the global illumination ray scene stay as they are. With `INSTANCE_FLAG_RAY_SCENE_ONLY` and casting on,
an instance is traced by shadow and bounce rays only: a ray proxy for ground the camera already draws, which must not
draw that ground's shadow a second time. `INSTANCE_FLAG_RAY_SCENE_ONLY` alone is unchanged, so a camera-hidden
instance that still casts raster shadows (a room hidden by portal culling, for one) keeps casting.

- `servers/rendering/renderer_scene_cull.h:InstanceData`: `FLAG_NO_RASTER_SHADOW = 1 << 26`; `Instance::no_raster_shadow`
  (default false). Three rules live here so every site reads the same one:
  - `casts_raster_shadow(casts, no_raster_shadow)`, and its flags form `casts_raster_shadow(flags)`: casting on and
    the new flag off.
  - `draws_in_camera(flags)`: the ray-scene-only flag off.
  - `enters_ray_scene(flags)`: the base is a mesh. Neither flag takes part.
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::instance_geometry_set_flag`: the new case sets the
  flag on the instance and on its InstanceData when the instance is in a scenario. Any base is accepted. It has an
  effect only on geometry bases (mesh, multimesh, particles), since every site that reads it sits behind a geometry
  check; on other bases it does nothing.
  When the flag changes on a casting instance, it marks the shadows of every paired light dirty once, because the set
  of raster casters changes then.
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::_update_instance`: at instance add, the flag is copied
  into InstanceData.
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::_scene_cull`: the directional cascade caster cull uses
  `casts_raster_shadow(flags)`; the camera branch uses `draws_in_camera`; the ray scene admit uses `enters_ray_scene`.
- `servers/rendering/renderer_scene_cull.cpp:RendererSceneCull::_light_instance_update_shadow`: the omni (paraboloid
  and cube), spot and area caster culls use `casts_raster_shadow(casts, no_raster_shadow)`.
- Lamp dirty marking: a casting instance marks its paired lights' shadows dirty when it is re-boxed or rebuilt
  (`_update_instance`), paired or unpaired (`_instance_pair`, `_instance_unpair`), its layer mask changes
  (`instance_set_layer_mask`), its transparency changes with dithered fade (`instance_geometry_set_transparency`) or
  its materials change whether it can cast (`_update_dirty_instance`). With the flag set, none of these mark lights
  dirty, since the instance draws nothing into their maps; a streamed ground cell that is rebuilt does not redraw lamp
  shadow maps. Light pairing itself is unchanged.
- Static meshes only, when used with `INSTANCE_FLAG_RAY_SCENE_ONLY`. `_scene_cull` asks for a skinned or blend-shape
  mesh's per-frame update (`mesh_visible`) only from the camera branch, the cascade caster cull and the SDFGI region
  cull. An instance with both flags is in none of the first two, so its mesh instance is not updated and the ray
  scene would keep tracing a stale pose. The ray ground this flag was added for is static.
- Ray scene: membership and the ray flags are unchanged. With casting on, `RayScene::shadow_cast_flags` gives no pass
  flag, so both shadow and bounce rays stop on the instance; `SHADOW_CASTING_SETTING_SHADOWS_ONLY` would pass bounce
  rays and drop its bounce light.
- `doc/classes/RenderingServer.xml`: `INSTANCE_FLAG_NO_RASTER_SHADOW` entry; the `INSTANCE_FLAG_RAY_SCENE_ONLY` entry
  now points to it for keeping a proxy out of the raster maps.
- Tests: `tests/servers/rendering/test_no_raster_shadow_flag.cpp` checks the three rules, not the call sites: it
  proves what each rule answers, and the sites above are where the code calls them. Known answers for flags with both
  set (in the ray scene, out of the camera cull, no raster shadow by either caster rule, which is also the rule the
  dirty marking calls), with only the ray-scene-only flag (still casts raster shadows), and with only the new flag
  (still drawn). Each rule reads only its own flags.
