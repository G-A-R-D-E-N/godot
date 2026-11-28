# Review rules for the Garden godot fork

These come from what the team fixed before merge in recent pull requests (ReSTIR direct lighting, the ray scene, direct shadows). Report a problem in these areas whenever the code shows it. They never replace judging the code itself.

## paths: **/*.glsl, **/*.glslinc, modules/gi_restir_di/**, servers/rendering/renderer_rd/**
- CPU and shader layouts must agree: packed bit offsets, field widths (a 16-bit truncation of an index that is compared as 32-bit), buffer sizes and array lengths. Report any index read from a buffer without a bounds check against the current count.
- A pass that reads and writes the same buffer needs a barrier or a second buffer. Report history that is only partly written (Quarter or block resolve) but read at full size, and a resolve that reuses one texel for the whole block.
- Report counters or values that are computed or logged but never updated or read, and dead code left after a removal.

## Engine hygiene
- Debug prints, per-frame string allocation, synchronous GPU readbacks, full-frame CPU copies, ungated debug atomics and project-specific logging must not ship in the render path.
- Report pointer-to-int casts that break a 64-bit editor build, tests without an `RD_ENABLED` guard, and misindented code that fails the `clang-format` gate.
- When a texture or scene changes, `data_version` must be bumped on every update path, and the scene hash must cover every field the resampler reads.
- Report resources freed only when a related buffer is valid, and RIDs that are allocated but not released.

## Text encoding
- Non-ASCII characters (multiplication signs, enum labels, class reference XML) are easily turned into mojibake by an edit. Report any such corruption.

## paths: docs/**, doc/**, .github/**
- Code notes must name the bits, paths and symbols the code now uses.
- Dependabot must not bump deliberately old Roslyn pins, and its GitHub Actions entry must cover composite actions in `.github/actions`.
