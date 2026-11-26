# Godot code notes

### `servers/rendering/renderer_rd/environment/direct_shadows.cpp:DirectShadows::get_history`

Quarter resolve produces a full-resolution history guide in addition to its block guides. Each resolved pixel records its own depth and normal, including coherent blocks whose lighting is broadcast, so the direct resampler can safely reuse Quarter output when switching modes. Empty, sky and out-of-region pixels have an invalid guide. This adds eight bytes per pixel and one guide write per resolved pixel; the current change still needs GPU validation for odd sizes, both resolve settings, resize and disocclusion.
