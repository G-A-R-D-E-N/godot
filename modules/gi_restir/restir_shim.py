import os
import re

SHIM_NAME = "restir_core_inc.glsl"
CORE_INCLUDES = (
    "ray_scene_inc.glsl",
    "rtgi_inc.glsl",
    "ray_scene_hit_shade_inc.glsl",
)
EXTRA_INCLUDES = ("ddgi_inc.glsl",)
PER_FILE_PREFIX = "restir_core_"
INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"', re.MULTILINE)


class ShimError(Exception):
    pass


def _read(path):
    with open(path, "rb") as handle:
        return handle.read()


def _write_if_changed(path, data):
    if os.path.isfile(path) and _read(path) == data:
        return
    with open(path, "wb") as handle:
        handle.write(data)


def _norm(path):
    return os.path.normcase(os.path.abspath(path))


def check_same_drive(shader_dir, build_dir):
    shader_drive = os.path.splitdrive(os.path.abspath(shader_dir))[0].lower()
    build_drive = os.path.splitdrive(os.path.abspath(build_dir))[0].lower()
    if shader_drive != build_drive:
        raise ShimError(
            f"gi_restir: the module shader folder ({shader_dir}) and the engine build folder ({build_dir}) are on different drives. "
            "glsl_builders.py resolves includes with os.path.relpath from the build folder, which cannot cross drives. "
            "Put the module folder on the engine drive."
        )


def include_closure(core_dir, names=CORE_INCLUDES + EXTRA_INCLUDES):
    found = {}
    pending = [os.path.join(core_dir, name) for name in names]
    while pending:
        path = pending.pop()
        key = _norm(path)
        if key in found:
            continue
        if not os.path.isfile(path):
            raise RuntimeError("core include missing: " + path)
        found[key] = path
        for match in INCLUDE_RE.finditer(_read(path).decode("utf-8")):
            inc = match.group(1)
            if inc.startswith("thirdparty/"):
                continue
            pending.append(os.path.join(os.path.dirname(path), inc))
    return [found[key] for key in sorted(found)]


def _shim(core_dir, shader_dir):
    shim_path = os.path.join(shader_dir, SHIM_NAME)
    lines = []
    for name in CORE_INCLUDES:
        source = os.path.join(core_dir, name)
        if not os.path.isfile(source):
            raise RuntimeError("core include missing: " + source)
        rel = os.path.relpath(source, shader_dir).replace(os.sep, "/")
        lines.append(f'#include "{rel}"')
    deps = include_closure(core_dir)
    _write_if_changed(shim_path, ("\n".join(lines) + "\n").encode("utf-8"))
    for name in CORE_INCLUDES + EXTRA_INCLUDES:
        source = os.path.join(core_dir, name)
        rel = os.path.relpath(source, shader_dir).replace(os.sep, "/")
        _write_if_changed(os.path.join(shader_dir, PER_FILE_PREFIX + name), (f'#include "{rel}"\n').encode())
    return deps


def write_shim(core_dir, shader_dir, build_dir):
    check_same_drive(shader_dir, build_dir)
    os.makedirs(shader_dir, exist_ok=True)
    return "shim", _shim(core_dir, shader_dir)
