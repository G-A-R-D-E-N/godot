# DI-1 trace resolution: Full and Quarter

### `servers/rendering/renderer_rd/environment/direct_shadows.cpp`

`direct/trace_resolution` is read every `process()` call. Full is today's per-pixel trace and filter, unchanged in what
it writes. Quarter runs the trace and the three filter passes on a block grid of `ceil(size / 2)`, one thread per 2x2
block, and then a resolve pass writes the full-size slots and factors that shading reads.

Quarter is used only when the setting asks for it, `precise()` is false (no `direct/reference`, `history_cap` above 0),
and its shaders were made. Quarter has its own `ShaderRD` objects for the trace (software and hardware), the filter and
the resolve, apart from Full's. A version is valid only when every variant in it compiles, so a Quarter variant inside
Full's versions would let a Quarter compile failure take Full down too, and on hardware rays refuse them. A failure
prints one warning and that frame runs Full. The hardware Quarter trace is made right after Full's hardware pipelines
on the first hardware frame, and the software ones at construction, so switching the setting makes no pipeline.

`_ensure_textures` compares the effective resolution with entries, size and precise, so a change clears the context
and starts history over in the frame it is read. That frame is complete: trace, filter and resolve run in it.

In Quarter the history textures keep their names (`slots`, `accum`, `rest`, `guide`, both sets), `scratch` and `ray`, but
are made at the block grid. `factors_block` holds the trace's or filter's block factors. `factors` stays full size and
is written by the resolve, so `get_factors()` is unchanged; `get_slots()` returns `slots_resolved`, a single set rewritten
whole every frame. The debug texture is made at the block grid and the existing copy stretches it.

`Params.size` is the grid the trace and filters run on. `full_size`, `pixel_size` and the cluster values stay on the full
size, because each block reads its pixels and looks lamps up at full resolution. `world_per_pixel` is per grid texel,
so it doubles in Quarter, and the kernel floor halves to keep its length on screen. `Params` is 384 bytes in both modes
(a static assert holds it); Full fills `full_size` and `jitter_delta` and reads neither.

`jitter_delta` is the previous frame's TAA or FSR2 jitter minus this frame's, in pixels. The jitter is an NDC offset
(`get_cam_projection()` adds it to the projection), so its pixel size is `jitter * size / 2`. The stored previous
projection carries the previous jitter, so the same pixel reprojects by exactly this delta on a still camera. The last
jitter is kept in `ViewportData` next to the last projection it belongs to.

`direct/filter_second_pass` off (Quarter only) runs the first pass in its final form, `FIRST_FINAL`: it writes decoded
factors to `factors_block`, 1.0 where there is no evidence, with `force_factor_one` and debug view 6 as the second pass
has them.

Counters are 13 words. Word 8 counts history accepts in both modes (pixels in Full, block threads in Quarter); words 9 to
12 are the resolve's tier 1, tier 2 and tier 3 pixels and pixels whose lamp list was cut, over non-sky pixels inside the
ray scene's region. In Quarter words 0 to 7 count block threads.

The dump report gains `mode` and the new counter keys in both modes. A Quarter dump also has `block_width`,
`block_height` and `inv_projection` (16 floats, column-major, the inverse of the frame's projection as the passes used
it), and its files are `slots` and `factors` (resolved, full size), `slots_block`, `factors_block`, `accum_block`,
`rest_block`, `guide_block`, and `depth_full` (R32F, the view depth the resolve used), `normal_full` (the raw
normal-roughness texel, `packUnorm4x8`) and `tier` (R8UI). The last three textures exist only from the first counted
frame on; before that 1x1 stand-ins are bound and the resolve writes them only when `count` is set.

### `servers/rendering/renderer_rd/shaders/environment/direct_shadows.glsl`

`TRACE_QUARTER` (capped history only) picks the block's representative: the nearest non-sky pixel is R; a pixel is on
R's surface when `exp(-plane / max(0.01 * depth_R, 1e-4)) * max(dot(n_R, n_P), 0)^2 >= 0.25`; a smooth block takes its
first non-sky pixel; a mixed block takes the first pixel on R's surface when `(b.x + b.y + (b.y >> 1)) & 1` is 0 and the
first one off it when 1. The positions in that test are rebuilt from the linear depth the way the filter's `guide_from`
does, so the CPU mirror can repeat it from a dump.

Everything after the pick runs as Full does at the representative's full-resolution pixel, and every store goes to the
block. The guide's depth word carries the phase (`ox + 2 * oy`) in its two low bits, so empty is `(word & ~3u) == 0u`
in Quarter.

History in Quarter blends candidates: blocks k with `2k + 0.5 < x + 2` and `2k + 1.5 > x - 2` per axis (at most nine),
where x is the reprojected position, unfloored, less `jitter_delta`. Each passes today's depth and normal tests on its
own guide and is weighted by the tent at x of its stored representative. Per own lamp the hits and the count are summed
over the candidates that hold it and divided by the weight of all passing candidates, so a lamp only a light candidate
holds comes back with a small count and the doubt term traces it again. The rest record blends the same way over the
candidates with the same rest key. When x lies within 0.01 px of this frame's representative on both axes and the own
block passes, it is used alone.

### `servers/rendering/renderer_rd/shaders/environment/direct_shadows_resolve.glsl`

One thread per block, 8x8 per group, a 12x12 block tile in shared memory (guide 2 words, slots 4, factors 5). A group
whose tile has no non-empty guide writes slots 0 and factors 1 at once, except on counted frames, so the tier counters
see every pixel.

Per pixel: sky or past the region gives slots 0 and factors 1 (tier 0). Tier 1 uses blocks `(F - 1) >> 1` and the next
one per axis, weight `tent * g`, with g the representative test above taken with F as centre and the block's position
rebuilt at its representative pixel; only blocks with g >= 0.25 count. Tier 2, when tier 1 has none, the 4x4 blocks from
bracket - 1 to bracket + 2 within 3 px, weight `g / (1 + d^2)`. Tier 3: slots 0, factors 1.

Lamps: score = sum over used blocks of `weight * (own_limit - rank)`; the own_limit best scores are kept (ties: lower
slot). A kept lamp's factor is the weighted mean over the used blocks that hold it as own. The rest record is the key
with the largest summed weight (ties: lower key) and the weighted mean of its three factors over the blocks with that
key. The merge holds at most 32 distinct lamps; more count as a cut list.

### `servers/rendering/renderer_rd/environment/direct_quarter.h`

The CPU mirror of the representative rule, the resolve and the history candidates and blend, in the same order and
arithmetic as the shaders. `tests/servers/rendering/test_direct_quarter.cpp` checks it, odd sizes included.
