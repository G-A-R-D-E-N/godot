# Ray-scene light pick: Camera and Stable modes (PBR F26)

Design rev 7, for `servers/rendering/renderer_rd/environment/ray_scene.cpp` on
engine darkstar/dev a781e25c5.

### `servers/rendering/renderer_rd/environment/ray_scene.cpp`

The ray scene packs at most `ray_scene/max_lights` lights (directional first, then local) into the buffer the DDGI
trace loops over. `ray_scene/light_pick_mode` decides which local lights survive when more reach it. The types
(`PickKey`, `PickSlot`, `PickState`, `PickParams`, `PickCensus`) and the new `LightCensus` fields are in
`ray_scene.h`.

**Camera** runs the 773f9349d selection on the current candidate list. `_select_lights` is one call to the static
`select_nearest`, whose body is the old `_select_lights` body with the members turned into arguments and
`MAX_LIGHTS` / `RESERVED_AREA_LIGHTS` taken from `max_lights` / `reserved_area_lights`. The only additions inside
it are census counters that read lights and write new census fields. Candidates are still capped at
`MAX_CANDIDATE_LIGHTS` in arrival order, the light region is the camera-centred mesh region, and `LightOrder` still
orders the buffer. With `max_lights` 256 and `reserved_area_lights` 16 it is today's pick, byte for byte (doctest
"Camera light pick is the old pick").

**Stable** scores lights from the camera's pick cell, not from the camera:

- The world cell key is `floor(camera / G)`, `G` = `ray_scene/light_pick_cell` (8, 16, 32, 64; any other value reads as
  16). Every value divides the 64 m anchor grid, so `cell_origin = key * G - anchor` is exact.
- Keys, in order: importance descending, distance score ascending, kept first, light-instance RID id ascending, then
  the candidate index (reached only when two candidates share a RID). Importance is `light_pick_power` (energy x
  luminance, x area for area lights, 0 for negative, zero or non-finite) times the shader's omni falloff evaluated at
  the distance from the light to the cell box, with that distance floored at `G / 2` for the power term. The distance
  score is `LightNearer::score` from the cell centre. A light with a non-finite position gets importance 0 (without
  that, `MAX(NaN, 0.0)` reads 0 and the light would score as if it sat inside the cell). A NaN importance reads as 0
  and a NaN distance as `FLT_MAX`, so the comparator stays a strict weak order. `PickOrder` (the slot order) compares
  each position axis and `inv_radius` with NaN sorted after every number and equal to another NaN, so it is a strict
  weak order too. A plain `<` would make a NaN-positioned light equal to everything on that axis, which breaks
  transitivity, and `SortArray` can then run past the range.
- A light the slot already keeps has its importance multiplied by `1 + light_keep_share` and its distance lowered by
  `light_keep_distance` (negative = `G`). The kept flag comes after the distance, so zero-importance kept lights do
  not freeze the set.
- `pick_set` keeps the two-stage shape of the old cut: up to `reserved_area_lights` area lights by the same keys
  inside the area class, the other area lights join the main pool. `select_best` runs `nth_element` and then checks
  that the worst kept key is not beaten by the best cut key; if it is, it sorts the whole array. `SortArray`'s
  introselect falls back to `partial_select` at its depth limit, and that path leaves the k-th best at the front of a
  heap rather than at index k, so an exact partition is not guaranteed there.
- The selected lights are packed sorted by (position x, y, z, `inv_radius`, RID id), so coincident lamps keep their
  slot order whatever order the cull merged them in.
- Stable packs every candidate (`_pack_light` with no limit); the pick does the cut.

**Slots.** `PickState` holds four `PickSlot`s, each with a cell key, a region key, the last camera, an LRU serial
and its kept RID set, plus `last_packed` (local indices of the last packed local lights). Stable updates it in every
`pick_lights`; Camera updates it only with verbose stdout, its one reader there being the `RAYSCENE_PICK` line, so the
Camera path does no extra sorting in a normal run.
`RayScene::match_slot(state, camera, G, serial)` matches by camera, not by cell key:

1. Among used slots whose widened region range `[r G - G/2, (r + 1) G + G/2)` holds the camera on every axis, the
   most recently used one. Its region does not move.
2. Otherwise, among used slots whose last camera is within `G / 2` of this camera on every axis, the most recently
   used one. This is the walking view; its region may step.
3. Otherwise a free slot, or the least recently used one is evicted. The new slot's kept set is copied from the used
   slot nearest by cell-key Chebyshev distance (most recent on a tie), never from the slot being evicted; the region
   key is set to the cell key.

The matched slot takes the new cell key, camera and serial. Why not "within one cell": two views in adjacent cells but
more than about 1.5 cells apart cannot both lie in one widened range, so a shared slot would move its region key on
every update and change the candidate set each time. With the rule above two views share a slot only when one lies in
the other's widened range, or within `G / 2` of it; in the second case the region steps once and then holds both,
because a region re-keyed to `floor(camera / G)` holds every point within `G / 2` of that camera. A single view that
moves less than `G / 2` per update keeps one slot. A larger jump that leaves the range takes a new slot seeded from
the old one, with the region key the old slot would have stepped to.

The LRU serial is `update_serial`, bumped once per `update()`, because two views can render in one frame.
`PickSlot::fresh` is set when `match_slot` allocates a slot and cleared by the next `pick_lights`; it is what
`slot_new` reports, since the match may happen in `get_light_region` before `update()`. `_pick_settings` clears all
four slots (not `last_packed`) and the recorded region when the pick cell size or the mode changes, so keys of two
cell sizes are never compared and a Stable, Camera, Stable switch starts clean. Cell keys and region keys come from
`cell_index`: `floor(x / G)` clamped to the int32 range, 0 for a non-finite value.

**Light region.** `RayScene::get_light_region` (public, takes `mesh_mutex`) matches the slot with the serial the next
`update()` will use, runs `light_region`, and records `{camera, half, slot}`. `update()` uses the recorded slot when the
camera is the same; otherwise it calls `match_slot` itself and does not move the region key. `light_region` moves
the region key on an axis only when the camera leaves `[r G - G/2, (r + 1) G + G/2)`, then returns the box centred on
the region cell with half = mesh-region half + `G`. For any camera in that widened range the camera-centred mesh
region lies inside the light box, so Stable loses no light that Camera mode would see.

**Census.** `LightCensus` gains the class split (`cut_area`, `cut_nonarea`, their powers, `packed_area_power`,
`packed_nonarea_power`), `held`, `pick_cell`, `region_cell` with `has_region`, `pick_mode` and `max_lights`; every one
is in `operator!=`. Powers are `light_pick_power x inv_exposure`, `inv_exposure = 1 / exposure` (0 when exposure is
not positive), computed once in `update()`. The masked fields keep `light_power` on packed energy. Totals and shares
are derived in `_print_census` only. `held` (kept lights the margin-free pick would cut) is computed only with
verbose stdout, as is the candidate diff. The margin-free pick is the pick with `light_keep_share` 0 and
`light_keep_distance` 0: it keeps the kept flag as the third key, so a kept light that wins only an exact tie on
importance and distance is not counted as held.

**RAYSCENE_PICK** (verbose only) is printed after `DDGILightTracker::end()` when any of `slot_new`, `cand_in`,
`cand_out`, `cand_changed`, `in`, `out`, `slot_in`, `slot_out`, `fired` is nonzero or the cell, region or slot
changed. `cand` and `pending` are levels and are printed but do not trigger the line. `fired` is the
`DDGILightTracker::stats().fired` delta across the update; `pending` is `pending_count()` after `end()`. Both count
only with `ddgi/light_change_reset` on (the tracker adds `reset_on` to `fired` and clears `pending` when it is off),
so with it off they print `-`. The candidate
diff (`RayScene::_candidate_changes`) compares (local index, 64-bit hash of the packed light with energy zeroed,
energy x `inv_exposure`) against the previous update; a scaled energy change above 1e-5 relative counts as changed.

`observe` runs before the cut, in both modes, so a pick swap never fires a tracker box. In Camera mode it runs on every
candidate. In Stable mode it runs only on candidates whose stored instance box (`light_instance_get_base_aabb`, the
box the cull tests, margin included) overlaps the camera-centred region. `update()` builds that region as
`tracker_region` with the static `tracker_region_for` from the camera and the half that `get_light_region` recorded
in `region_half`; it is empty in Camera mode or when no half was recorded, and an empty region observes everything.
`_pack_light` asks the static `tracker_observes`: true for a directional light (`observe` skips it itself), for an
empty region, or when the box overlaps the region, with the same open bounds as the cull's `in_aabb`. So below
`MAX_CANDIDATE_LIGHTS` the observed set in Stable mode is exactly the Camera mode candidate set, and the tracker
fires the same events: a light that crosses the camera region's face, or is freed or hidden, fires, and a region step
fires nothing on its own. Without this gate a step put every lamp of the entering slab into one frame and every lamp
of the leaving slab into the next (a light is gone one frame after its last observation); past
`DDGILightTracker::MAX_EVENTS` (64) the ring drops events, `rtgi.cpp` reports them as lost and ReSTIR GI takes a jump
frame (`modules/gi_restir/restir_events.cpp`, `events_lost > 0`; VALIDATE_ALL when it has history and `jump_m_cap` is
above 0).

Event limit: a Stable candidate outside the camera region fires no change event until it enters that region. Its box
bounds its range, so it cannot light a hit inside the region. It can light a hit on a mesh part outside the region
(the ray scene holds meshes that only overlap it), and a change to that light is not tracked there; Camera mode never
lit those hits at all, so this is a limit of Stable, not a regression. It is still packed, picked and traced (see the
light box bullet below).

Doctest "Stable light region steps stay under the tracker ring, and their events match Camera mode": 4620 lamps, a 60 m
walk in 0.5 m steps over three region steps, driven through `light_region`, `tracker_region_for` and
`tracker_observes`. Gated, at most 36 events a frame, none lost, and the per-frame counts equal Camera mode's. Two
mutants must fail it: the gate fed the snapped light box fires 160 events in the step frame and again in the next (six
frames, 96 lost in each, 576 in all), and the gate fed last frame's camera moves events by a frame. It also pins the
open bounds: face contact is not an overlap. No doctest builds a `RayScene`, so the wiring is not reached: the gate
call in `_pack_light`, the `tracker_region` assignment in `update()` with its camera argument, and the `region_half`
write in `get_light_region` (without it the gate observes everything). The older doctest "An ungated tracker sees one
burst of DDGI tracker boxes per Stable region step, and a dither across the edge releases none" feeds the tracker
directly, without the gate.

**Facts to keep** (from the design; the source carries no comments):

- The shipped defaults are a deliberate change: Stable mode, `max_lights` 1024, `reserved_area_lights` 16. Off for
  every on/off pair is Camera with `max_lights` 256. `MAX_LIGHTS` (256) in `ray_scene.h` is only the Camera-identity
  value and the member's initial value; the setting is read on every update. The tiers are game-side: Baseline and
  Standard 1024, High 1024 or 2048 (2048 when the whole-game census finds a view over 1024), Reference 4096 or more,
  never lowered for cost.
- Per-ray cost: the DDGI hit loop runs over every packed light (one 96 B broadcast load, the attenuation math and
  one hash each). Shadow rays do not grow with the light count (at most 4 exact, else one stochastic ray, plus one
  per directional), and BVH traversal is unchanged, so the cost is linear ALU. Estimate at 4096 probes x 128 rays:
  849 lights about 0.27 ms ideal on a 4090, 2048 about 0.65 ms, 4096 about 1.3 ms; divergence may double or triple
  these. Cost is measured in the on/off pair, never from a light-load reading. More lights add variance only where
  more than 4 lights reach a hit.
- The light box is 1.48x the camera region's volume at G = 16 (320 x 192 x 320 against 288 x 160 x 288). Its margin
  around the camera region is not uniform: the camera may sit up to `G` from the box centre on an axis, so the margin
  is `G` on each side when centred and up to `2 G` (32 m at G 16) on one side. Up to about 48 % more candidates
  are packed, picked and traced when candidates are fewer than `max_lights` minus the directional lights, with the
  per-ray cost above; the tracker records only the camera region's lights. Stable packs every candidate: about 6.4 MB
  transient at 50,000 candidates (96 B light + 32 B key).
- Below `MAX_CANDIDATE_LIGHTS`, Stable mode fires the same tracker events as Camera mode (the gate above); a region
  step fires no burst. Pending boxes over 1024 merge into the nearest one and `take` hands out 32 a frame; none are
  dropped.
- The light index seeds the shader's noise, so one inserted light reshuffles the noise of every light after it in the
  buffer. It stays unbiased and changes only on swap frames.
- The tracker's energy band compares packed energy, which includes the camera-attributes exposure, so animated
  camera attributes would reset every light. Known and out of scope here.
- `light_pick_power` is not `light_power`: it multiplies area lights by their area and clamps negative, zero and
  non-finite power to 0. `light_power` would give an area light energy x luminance and a negative light a negative
  power. The cut census is split by class because a lit derived area panel turns its mesh's field emission off, so
  cutting that light removes its bounce entirely rather than one of two paths.
- Census powers are float `energy x exposure` times double `1 / exposure`, exact only for power-of-two exposures;
  other exposures can move the low bits and reprint the census once (verbose only).

### `servers/rendering/renderer_rd/renderer_scene_render_rd.cpp`

`ray_scene_get_light_region` (declared in `renderer_scene_render.h` and `renderer_scene_render_rd.h`) is non-const
because the Stable path advances slot state. The base returns `ray_scene_get_region`. The RD override returns the
mesh region unchanged when it is empty or in Camera mode, and otherwise passes the mesh region's half to
`RayScene::get_light_region`.

### `servers/rendering/renderer_scene_cull.cpp`

`Cull::ray_scene_light_region` (`renderer_scene_cull.h`) is set beside `ray_scene_region`, empty for reflection
probes. The ray-scene cull test is split by base type: meshes test `ray_scene_region`, non-directional lights test
`ray_scene_light_region`; the layer and visibility checks are shared. In Camera mode the two boxes are equal, so the
candidates are today's.

### `servers/rendering/rendering_server.cpp`

`ray_scene/max_lights` (16..16384, 1024), `reserved_area_lights` (0..16384, 16, clamped to `max_lights` and then to the
local budget), `light_pick_mode` (Camera, Stable; Stable), `light_pick_cell` (8, 16, 32, 64; 16), `light_keep_share`
(0.25), `light_keep_distance` (-1 = the pick cell). All are read on every ray scene update. Off for on/off pairs is
Camera with `max_lights` 256.

### `tests/servers/rendering/test_ray_scene_lights.cpp`

Cases 1-15 of the design, plus three: two views 1.9 cells apart keep their own slots and regions while a walking
view keeps one slot; the Stable census scales by `inv_exposure` while the pick does not; NaN energy, NaN position
and infinite position rank last. Test 10 also steps y and z. `golden_select` is the 773f9349d `_select_lights` body
with its own `LightNearer`, `LightOrder` and `light_power`, verbatim except that the two limits are parameters.
Every hand-built light starts from `Light l = {}` and sets `caster_mask` and `pad`. `PickInput` is `PickParams` plus
the camera the test helper passes to `match_slot`. Test 8 asserts the known answer w = 4 (the 20 omnis and 4 extra
panels with nonzero importance are far fewer than the free slots), so its identities are checked at that w only.
Test 10's Camera-mode check (`ray_scene_get_region` at (1,1,1)) needs the renderer singleton and is left to the
first GPU or check-only run. The `select_best` full-sort fallback has no test: introselect reaches its depth limit
only on adversarial input, which a fixture cannot build reliably.
