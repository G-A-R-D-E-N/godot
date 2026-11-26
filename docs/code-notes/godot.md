# Godot code notes

### `modules/mono/glue/GodotSharp/GodotSharp/Core/DisposablesTracker.cs:OnGodotShuttingDownImpl`

Shutdown closes native-finalizer admission after the initial drain, retains tracked wrappers through the sweep, and waits for admitted native calls before teardown. Replacements created synchronously by an admitted finalizer remain pending until that call returns. Registrations made by unrelated threads after the final tracker snapshot are not covered; the previous snapshot-based sweep had the same limitation. A permanently stalled disposal owner or admitted native call can prevent shutdown from finishing because returning while it can still touch native state would be unsafe.

### `modules/mono/glue/GodotSharp/GodotSharp/Core/GodotObject.base.cs:DisposeOwned`

When a finalizer has already entered an override but reaches base disposal after admission closes, the shutdown owner completes the deferred native release without invoking that override a second time. This follows finalizer semantics: an override's `disposing == true` branch is not called for an object already being finalized. The finalizer override can observe a nonzero native pointer after its deferred base call returns; the native owner remains held until the shutdown sweep completes the release.

### `servers/rendering/renderer_rd/environment/ray_scene.cpp:RayScene::_alpha_entry`

For inverted opacity ranges, a cutoff below zero discards every texel, while a cutoff at or above one retains every texel. Forward ranges use the opposite boundaries; a cutoff exactly at one can still retain the maximum-alpha texel.
