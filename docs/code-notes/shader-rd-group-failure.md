# ShaderRD::_compile_version_end: a failed group fails alone

A group that is off when a version is initialized gets placeholder shaders and compiles later, after
`ShaderRD::enable_group` (a lazy group). Before this change a failed variant in any group freed every enabled variant
of every enabled group, cleared `variants` and `variant_data`, and left `valid` as it was. A lazy group failing on a
valid version then made `ShaderRD::version_get_shader` index an empty `Vector` (an engine abort), killed the pipelines
built from the base group, and a lazy variant whose GLSL did not compile kept its placeholder and was written to the
user cache with a zero-size entry.

### `servers/rendering/renderer_rd/shader_rd.cpp:ShaderRD::plan_group_end`

The decision taken when a group's compile or cache load is collected, as pure logic so doctests can drive it without a
device. Input per variant of the group: enabled, shader not null, bytecode size. Input per group: `valid` before,
the sticky `creation_group_failed`, whether the group is lazy, whether the user cache folder is usable, and whether
the group came from the cache.

Rules:
- A group fails when an enabled variant has a null shader or no bytecode. A disabled variant never fails a group: its
  slot is `RID()` or a placeholder and its size is 0 on purpose (the cache stores 0 for it and `_load_from_cache` skips
  it).
- On failure every non-null slot of the group is listed for freeing, enabled or not, in ascending order: real shaders
  of sibling variants that compiled, kept placeholders, and placeholders of disabled variants of a lazy group.
- A failed group that is not lazy (enabled when the version was initialized) sets `creation_group_failed`, which
  stays set until the next `_initialize_version`.
- `version_valid = !creation_group_failed && (valid before || group ok)`. With no failure this is today's
  `valid = true`. A lazy failure on a valid version keeps it valid. A creation-time failure keeps the version invalid
  whichever order the groups are collected in: a lazy group that compiles afterwards does not make it valid, and a
  lazy group that compiled first does not keep it valid.
- A group is saved to the user cache only when it compiled, the user cache folder is usable, and it did not come from
  the cache. A failed group is never saved, so no zero-size entry is written.

Lazy means "had placeholders at version initialization", so it depends on when `enable_group` ran relative to the
version's (re)initialization: a material created after `enable_advanced_shader_group` compiles the advanced group at
creation, and a failure there makes the material invalid; for a material created before, the same failure leaves it
valid with `RID()` advanced variants. This matches the old outcome for creation-time groups (invalid, when it did not
abort) and limits the new survive-alone behaviour to groups enabled at run time. Using `VariantDefine::default_enabled`
instead would have made the rule independent of timing, but would have let a failed group enabled before
`version_create` (volumetric fog's group, GI's VRS group) leave a version valid with dead variants.

### `servers/rendering/renderer_rd/shader_rd.cpp:ShaderRD::_compile_variant`

An empty `variant_data` slot is the failure mark. The three failures (GLSL to SPIR-V, SPIR-V to bytecode, RD shader
creation) each empty the slot and return; the shader slot is not written. On success the same RD calls run with the
same arguments and the same two writes happen in the same order (`variants`, then `variant_data`).

The slot keeps what it held on failure: `RID()` for a group compiled at creation, the placeholder for a lazy group.
`shader_create_from_bytecode_with_samplers` returns `RID()` before it adopts the placeholder (container parse, then
driver create, then adoption), so the placeholder is still a live, empty shader that `free_rid` can release. It must
stay in the slot because `free_rid` is render-thread only and this runs on a worker; it is freed when the group is
collected.

The two compile failures empty the slot instead of leaving it alone because `_load_from_cache` can copy entries into
`variant_data` and then give up (a zero-size entry, a short read). The group then compiles from source, and a
variant that fails would otherwise keep stale cached bytecode next to its placeholder and pass the group check.

The old `ERR_FAIL_COND` lines are gone; `compile_stages` and RD print the real cause, and `_compile_version_end`
prints the shader and group.

### `servers/rendering/renderer_rd/shader_rd.cpp:ShaderRD::_load_variant_from_cache`

Same mark for a cache entry that does not parse or that the driver rejects: the slot keeps its placeholder or `RID()`
and `variant_data` is emptied. The disabled-variant branch no longer writes `RID()` into the slot; it returns and
leaves the slot as it is, like `_compile_variant` does. Before, it overwrote a lazy group's placeholder for a disabled
variant with `RID()` without freeing it, leaking one placeholder per disabled variant every time a lazy group loaded
from the cache, and the failure path could not free it either. Now the placeholder stays in the slot and is freed by
`_clear_version` on success or by `_compile_version_end` when the group fails. For a group compiled at creation the
slot is already `RID()`, so nothing changes there. `version_get_shader` refuses a disabled variant before it reads the
slot, and every `set_variant_enabled` caller runs before `version_create`, so the kept placeholder is never returned.

### `servers/rendering/renderer_rd/shader_rd.cpp:ShaderRD::_allocate_placeholders`

Marks the group lazy in `Version::group_lazy`. `_initialize_version` resizes `group_lazy` and clears it, and clears
`creation_group_failed`, so both describe the current initialization only.

### `servers/rendering/renderer_rd/shader_rd.cpp:ShaderRD::_compile_version_end`

After the wait, reads the group's slots into a small `LocalVector` (no RD calls), asks `plan_group_end`, and on
failure prints one line naming the shader, the group number and whether it was compiled or loaded from the cache,
frees the listed slots, sets every slot of the group to `RID()` with empty bytecode, and stores the sticky flag and
`valid`. Nothing outside the group is touched: other groups' shaders, the RD objects built from them (pipelines,
uniform sets) and the `PipelineDeferredRD` objects holding them stay valid.

`variants` and `variant_data` are never cleared here any more, so another group's compile task still running writes
inside the vectors, and a later `_compile_version_end` for another group indexes a full-size vector. The failed
group's own tasks have finished (`wait_for_group_task_completion`) before its slots are read.

A failed group is not retried. Its task is 0 and `group_started` stays true, so its variants read `RID()` until the
next `version_set_*_code` re-initializes the version; then every enabled group compiles again.

`free_rid` is render-thread only. If a group is collected on a worker (forward clustered pipeline compiles call
`version_get_shader` there), each free prints the render-thread guard error and the shader leaks; the slots are
still reset, so there is no stale handle and no double free. The old teardown had the same limit.

A corrupt user cache entry is not deleted or replaced: every run fails that group again with "could not be loaded
from the shader cache". Falling back to a source compile, or removing the entry, is a follow-up.

### `servers/rendering/renderer_rd/shader_rd.h:ShaderRD::version_get_shader`

Unchanged. `variants` is cleared only by `_clear_version`, which is always followed by `_initialize_version`'s resize
(with `valid` false) or by `version_free`, so `valid` implies a full-size `variants` and the final index cannot run
past it. A failed lazy group's variants read `RID()` while the version stays valid; callers must test the returned
RID, not only `version_is_valid`.

### `servers/rendering/renderer_rd/environment/ddgi.cpp:DDGI::trace_rtgi`

The `TRACE_GROUP_RTGI` shader is read once, when the group is first enabled; the pipeline is made only from a
non-null shader. Every frame, a null RTGI pipeline returns false on both trace paths; the hardware path still calls
`refuse_hardware` first. Before, the software path went on to `_trace` with a null pipeline and null-shader uniform
sets every frame and returned true, so `RtgiPass` read last frame's `candidate_*` data as this frame's rays. Now it
refuses the frame through its existing "the trace did not run" path. With a working group the only difference is
that the software path waits for the pipeline compile here instead of inside `_trace`, in the same frame.

### Callers that still print on a failed group

- `SceneShaderForwardClustered::ShaderData::get_vertex_input_mask` and the mobile twin test `shader_rid.is_null()`
  with `ERR_FAIL_COND_V` on every call, because the mask is cached only on success. A failed multiview or advanced
  group prints once per surface update.
- `GI::process_gi` in `gi.cpp`: with VRS supported every GI pass uses the VRS variants, read each frame and passed to
  `uniform_set_create` and `get_cache_vec`. A dead VRS group prints every frame and breaks GI as a whole. The VRS
  group is enabled before `version_create`, so its failure makes the version invalid. Upstream code, not changed here.

### `tests/servers/rendering/test_shader_rd_group_failure.cpp`

Drives `ShaderRD::plan_group_end` only: no `RenderingDevice`, no `ShaderRD` instance. The worker-side slot handling,
the `free_rid` calls and the error line need a device and are not covered by doctests.
