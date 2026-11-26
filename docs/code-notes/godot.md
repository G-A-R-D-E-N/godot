# Godot code notes

### `modules/mono/glue/GodotSharp/GodotSharp/Core/DisposablesTracker.cs:OnGodotShuttingDownImpl`

Shutdown closes native-finalizer admission after the initial drain, retains tracked wrappers through the sweep, and waits for admitted native calls before teardown. Replacements created synchronously by an admitted finalizer remain pending until that call returns. Registrations made by unrelated threads after the final tracker snapshot are not covered; the previous snapshot-based sweep had the same limitation. A permanently stalled disposal owner or admitted native call can prevent shutdown from finishing because returning while it can still touch native state would be unsafe.

### `modules/mono/glue/GodotSharp/GodotSharp/Core/GodotObject.base.cs:DisposeOwned`

When a finalizer has already entered an override but reaches base disposal after admission closes, the shutdown owner completes the deferred native release without invoking that override a second time. This follows finalizer semantics: an override's `disposing == true` branch is not called for an object already being finalized. The finalizer override can observe a nonzero native pointer after its deferred base call returns; the native owner remains held until the shutdown sweep completes the release.

### `servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::_alpha_entry`

For inverted opacity ranges, a cutoff below zero discards every texel, while a cutoff at or above one retains every texel. Forward ranges use the opposite boundaries; a cutoff exactly at one can still retain the maximum-alpha texel.

### `servers/rendering/renderer_rd/environment/ray_scene_alpha.cpp:RayAlphaStore::update`

Alpha-layer source changes are detected by comparing the RD texture, dimensions, and CPU-update version separately. Each change receives a fresh revision so its coverage and far mip are refilled without relying on overlapping packed fields. Externally writable sources receive a new observation revision when requested by a view.

### `servers/rendering/renderer_rd/storage_rd/texture_storage.cpp:TextureStorage::_texture_2d_update`

CPU uploads advance the backing texture's content version even when the upload is made through a proxy. Version reads follow proxy chains so sibling aliases observe the same update. GPU-written content is not covered by this CPU-upload counter.

### `servers/rendering/renderer_rd/storage_rd/texture_storage.cpp:TextureStorage::texture_drawable_blit_rect`

Drawable blits advance the backing textures' content versions after the draw is submitted, so later ray-alpha cache updates refill changed targets. Render targets and externally written RD textures need separate invalidation.

### `servers/rendering/renderer_rd/storage_rd/texture_storage.cpp:TextureStorage::texture_has_external_writes`

A rendering-device texture supplied by a caller, a render target, or a texture whose mutable device handle was exported may change without a `TextureStorage` upload. Internal texture lookups do not mark a texture externally writable. Proxy queries follow the backing texture. The alpha cache must refresh these sources at each sampling point; externally submitted writes must be synchronized with rendering.

### `servers/rendering/renderer_rd/environment/ray_scene_alpha.cpp:RayAlphaStore::update`

Externally writable alpha sources receive a new observation revision at each ray-scene update, including repeated views in one frame. A rotating fill plan reserves near and required far work together, while retaining the per-frame texel budget across views. Only dynamic sources requested by the current view when alpha testing is enabled consume that budget. The first requested job can exceed the budget to ensure progress; later views may remain pending when the shared budget is exhausted. Sources that cannot be refreshed remain pending rather than publishing an old mask.

### `servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::update`

The scene tracker incorporates the alpha table after refresh scheduling, so far-state and density changes are reflected in the same update that publishes them to rendering.
