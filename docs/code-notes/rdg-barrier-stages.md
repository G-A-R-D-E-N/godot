# Render graph: source stages that travel with source access

### `servers/rendering/rendering_device_graph.h`

`ResourceTracker::usage_stages` holds the stages that go with `ResourceTracker::usage_access`: those of the command
that made it, or the parent's when a slice copies the parent's access. Before this field, the access of a tracker's
last use was stored without the stages that made it.

### `servers/rendering/rendering_device_graph.cpp`

`RenderingDeviceGraph::_add_command_to_graph` sets `usage_stages` from the command's `self_stages` next to the final
update of `usage_access` for each use. A slice that takes the parent's access also takes the parent's stages, in both places where the
access is copied. Wherever a barrier uses a tracker's old access, that tracker's `usage_stages` is added to the
command's `previous_stages`. That covers the usage-change and write-after-write barrier and the normalization barriers that put slices back
to the parent's layout. Barrier levels and command order do not change.

The fault it fixes: a barrier took its source stages from the command's `previous_stages`, which are the stages of
the commands it depends on plus the resource's stages from the previous frame. Two paths broke that pairing:

- A slice use (a mip or layer range of a texture) updates only the slice's tracker. The parent keeps the access of
  its last full-resource use, possibly from an earlier frame.
- The first command to touch the parent in a frame takes `previous_frame_stages` and clears them.

A later barrier on the parent then paired the old access with source stages that cannot produce it. On Vulkan that
is `VUID-vkCmdPipelineBarrier-srcAccessMask-02815`. One known trigger is SSIL's previous-depth copies
(`SSEffects::ssil_store_previous_depth`) with `temporal_validation` on: the first copy's barriers carried TRANSFER
access under `TOP_OF_PIPE | COMPUTE_SHADER`.

What it does not do:

- It does not make the access more precise. It pairs the old access with the stages that made it.
- A command's `previous_stages` are merged into the source stages of its whole barrier group, so the wider source
  mask also reaches barriers in the group that do not use the old access. That is a small over-sync. It was not timed.
- D3D12: a mismatched pair used to fall back to the driver's SYNC_ALL. It now gets the narrower source scope of the
  real stages. No D3D12 run was made.
- Metal: the barrier call is a no-op, so nothing changes there.

How it was checked:

- Vulkan with validation on, on two GPUs. The message went to 0 in every scene set that showed it: lab scenes, an
  interior and an exterior view at two GI levels, a shore view in clear weather and rain, and a direct-shadow probe.
  No new validation message appeared.
- Pictures: byte-identical at the low GI level. At the high level, the differences are within the launch-to-launch
  noise of the unchanged engine.
