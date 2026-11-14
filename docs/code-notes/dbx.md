# Lane DBX: dirty-box feed in the ray scene

## What the feed is

`RayScene` publishes once per `RayScene::update`, while a consumer holds the feed, the world-space boxes of ray scene
instances that were added, removed, moved or whose mesh changed. Boxes are in world coordinates (`real_t`, double in this
build). They are never anchor-relative: a reader subtracts its own anchor. The record of an instance is compared on
`RenderGeometryInstanceBase::transform` (the world transform), not on the anchor-relative `Instance` record that goes
into the TLAS, so an anchor step (`ANCHOR_GRID`, 64 m) changes nothing in the feed.

## Accessor and consumers

| Symbol | File | Meaning |
|---|---|---|
| `RayScene::acquire_dirty_feed(FeedConsumer)` / `release_dirty_feed` | `ray_scene.h` | bit mask of holders (`FEED_SURFELS`, `FEED_DDGI`, `FEED_REFLECTIONS`, `FEED_RTGI`, `FEED_MODULE`, `FEED_DEBUG`). No holder: `RayScene::update` only tests one bool (`feed_on`) and the map is not kept |
| `RayScene::get_dirty_feed()` | `ray_scene.h` | `DirtyFeed` of the last update: `boxes` (merged, at most `dirty_box_cap`), `mover_boxes`, `overflow`, `baseline`, `structure_generation`, `serial`, `changes`, `tracked`, `mover_count` |
| `GIEpochs::get_feed_serial()`, `get_structure_generation()`, `is_feed_live()` | `gi_epochs.h` | the feed serial (one per update while the feed is held, never 0) and the structure generation, copied by `sync_ray_scene` |
| `GIEpochs::get_dirty_boxes_since_serial(serial, boxes)` | `gi_epochs.cpp` | boxes of the frames after `serial`, merged, at most 64 |
| `GIEpochs::get_dirty_boxes(since_scene_epoch, boxes)` | `gi_epochs.cpp` | the H-E form: boxes of the frames whose scene epoch is newer than the given one |

`DirtyResult` is `DIRTY_NONE` (nothing moved), `DIRTY_BOXES` or `DIRTY_OVERFLOW` ("anything may have changed": the feed is not
held, the ring does not reach back that far, a frame overflowed, or more than 64 merged boxes were needed). The
box list of an overflow result holds the first 64 merged boxes and is not a complete list.

## How the diff works (`ray_scene_dirty.cpp`)

- Key: the geometry instance pointer. State per key: world transform (compared bit for bit), mesh RID, `MeshEntry::generation`
  (changes whenever a mesh is rebuilt), the hardware-usable bit (an instance that the hardware TLAS cannot hold yet counts as
  absent for hardware rays), last world box (mesh AABB transformed by the world transform), 8-bit change history.
- `_feed_begin` (before the second instance loop of `RayScene::update`), `_feed_observe` (one call per instance that enters the
  ray scene, before `primitives.push_back`), `_feed_end` (after the TLAS work, before the final `sync_ray_scene`).
- New key: its box is pushed. Changed key: old box and new box are pushed. Key not seen in this update (checked only when the
  map is larger than the number of observed instances): its last box is pushed and the record is erased.
- The first update after the feed is acquired is the baseline: records are stored, no box is published, `baseline` is true.
  A reader that wants history starts from the serial of that update.
- Merge: `GIEpochs::merge_boxes` sorts by minimum x and merges boxes whose gap is smaller than `dirty_merge_distance`
  until nothing merges. More than `FEED_RAW_LIMIT` (4096) raw boxes in one update are not merged: the update overflows.
  More merged boxes than `dirty_box_cap` set `overflow` and keep the first `dirty_box_cap` (sorted by minimum x).
- Movers: a record whose history byte has at least `mover_frames` bits set in the last 8 updates is a mover; their boxes are
  published in `mover_boxes` (merged, capped). The history of idle records is not touched: only records that changed in the
  last 8 updates sit in a small active list.
- Skinned instances never reach `_feed_observe` (the loop skips them), instances whose mesh is not built yet are absent
  until it is.
- Material changes do not produce boxes. The instance class (opaque or candidate) is not part of the record either.
- `MeshEntry::generation` is taken from `mesh_generation_counter` in `_build_blas`, so it changes whenever a mesh is rebuilt,
  also when the mesh entry was erased and captured again.
- The hardware-usable bit is read from `hw_usable` right after `_hw_instance` (always true with the software backend).
- `update` starts by reading `debug_dirty_feed` (a cached setting) and holds or releases `FEED_DEBUG`; after that it only
  tests `feed_consumers` once.

## Structure generation

`RayScene::structure_generation` is bumped (a) in `_backend_tick` when the active backend flips (software to hardware or
back), and (b) in `_feed_end` when the previous update tracked at least one instance and none of them is still there (reload
or unload of the whole instance set; not bumped again if (a) happened since the last published update). A single instance
added or removed never bumps it. (b) needs the feed to be held; (a) counts always. This ray scene has no owner mask and no
`_invalidate_captures`, so there is no third bump site. `GIEpochs::sync_ray_scene(feed serial, structure generation)` copies
both at the end of every `update`.

## Ring (`GIEpochs`)

16 frames, at most `DIRTY_RING_BOXES` (32) boxes per frame, tagged with the serial, the scene epoch at the end of the update
and the structure generation. A read older than 16 frames, a slot that does not carry the expected serial (the feed was off in
between) or a feed that is not held gives `DIRTY_OVERFLOW`. The ring is cleared when the last consumer releases the feed.
Nothing on this branch bumps `GIEpochs::SOURCE_SCENE` and `RayScene` has no scene epoch, so the ring frames carry the word
as it is (0 until something bumps it). While the word has never moved, `get_dirty_boxes(since_scene_epoch)` returns
`DIRTY_OVERFLOW`, never `DIRTY_NONE`; it answers from the ring once something bumps the word. The serial query does not use
the word.

The baseline frame, the first update after the feed is acquired (also after a release and a re-acquire), is published with
overflow set (`RayScene::feed_frame_overflow`). The feed saw nothing while it was off, so a reader still holding a serial
from before the release reads `DIRTY_OVERFLOW`, never `DIRTY_NONE`.

## Settings (`rendering/global_illumination/ray_scene/`)

`dirty_box_cap` 32, `dirty_merge_distance` 0.5 (m), `mover_frames` 3. The ring keeps at most 32 boxes per frame, so a
`dirty_box_cap` above 32 gains nothing: every frame with more than 32 merged boxes reads as overflow. Internal: `debug_dirty_feed` 0 off, 1 hold the feed,
2 hold and write one JSON line per update to the file named by the internal `debug_dirty_feed_path` (probe use; no line is
written while the path is empty).

## Not covered

- Instances outside the ray scene region are not in the ray scene and so not in the feed: a camera that moves the region makes
  instances enter and leave at its edge, and those show as adds and removes.
- Several cameras that call `RayScene::update` with different instance sets in one frame make the same instances appear and
  disappear (the TLAS signature flaps the same way).
- `layer_mask`, `cast_shadows` and material edits change the TLAS signature but publish no boxes.
