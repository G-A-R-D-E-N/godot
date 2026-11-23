import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

# Packages the probe restores from the candidate engine's own nupkgs directory.
GODOT_PACKAGES = ["godot.net.sdk", "godotsharp", "godot.sourcegenerators", "godotsharpeditor"]

# The race case is a wall-clock-bound stress loop (20000 iterations of three threads disposing
# one object), so its duration follows the host's CPU count: about a second on a 12-core
# machine, about 16 seconds on the Windows CI runner, and 135 seconds pinned to two cores,
# all with the same PASS verdict. The budget is therefore more than twice the slowest
# measured machine, and the process timeout is longer still so that a case which overruns the
# budget is reported by the elapsed check instead of being killed. The other cases wait on
# fifteen-second gates that print a FAIL of their own when they expire, so the short budget
# only backstops them.
GATED_CASE_ELAPSED_LIMIT = 15
GATED_CASE_TIMEOUT = 40
LONG_GATE_CASES = {
    "zombie",
    "zombie-rewrap",
    "zombie-scripted",
    "zombie-scripted-child",
    "zombie-scripted-held",
    "zombie-scripted-ignore",
    "zombie-scripted-replace",
    "zombie-scripted-replace-child",
    "zombie-scripted-stress",
}
EXPECTED_WARNING = {
    "zombie-scripted-held": "Its new managed instance has default values.",
    "zombie-scripted-ignore": "Its new managed instance has default values.",
}
TOLERATED_WARNING = {
    "zombie-scripted-stress": "Its new managed instance has default values.",
}
# Must stay rare: many of these lines mean the finalizer is rebuilding wrappers nobody holds.
TOLERATED_WARNING_LIMIT = 10
# Must exceed the sixty-second gates in ZombieBinding.cs.
LONG_GATE_ELAPSED_LIMIT = 75
LONG_GATE_TIMEOUT = 90
STRESS_CASE = "race"
STRESS_CASE_ELAPSED_LIMIT = 300
STRESS_CASE_TIMEOUT = 360
WARMUP_TIMEOUT = 60
WARMUP_LINE = "WARMUP started and quit."

LEGACY = {
    "queued-held": [],
    "queued-drained": ["--release-before-quit"],
    "running-held": ["--running-finalizer"],
    "running-drained": ["--running-finalizer", "--release-before-quit"],
}
OWNERSHIP = {
    "overlap": "--overlap-finalizer",
    "prebase": "--prebase-finalizer",
    "race": "--race-dispose",
    "throw-shutdown": "--throw-recover-shutdown",
    "throw-finalizer": "--throw-recover-finalizer",
    "reentrant-throw": "--reentrant-throw-finalizer",
    "owner-api": "--owner-prebase-api",
    "zombie": "--zombie-binding",
    "zombie-rewrap": "--zombie-rewrap",
    "zombie-scripted": "--zombie-scripted",
    "zombie-scripted-child": "--zombie-scripted-child",
    "zombie-scripted-held": "--zombie-scripted-held",
    "zombie-scripted-ignore": "--zombie-scripted-ignore",
    "zombie-scripted-replace": "--zombie-scripted-replace",
    "zombie-scripted-replace-child": "--zombie-scripted-replace-child",
    "zombie-scripted-stress": "--zombie-scripted-stress",
}
CONTROL = ["throw-shutdown", "throw-finalizer", "owner-api"]
READY = {
    "overlap": "READY overlap finalizer held inside base disposal.",
    "prebase": "READY prebase finalizer held before base disposal.",
    "throw-shutdown": "READY throw-shutdown disposal failed before base disposal;",
    "throw-finalizer": "READY throw-finalizer disposal failed before base disposal;",
    "reentrant-throw": "READY reentrant-throw disposal failed before base disposal;",
    "zombie": "READY zombie binding set up:",
    "zombie-rewrap": "READY zombie rewrap set up:",
    "zombie-scripted": "READY zombie scripted set up:",
    "zombie-scripted-child": "READY zombie scripted child set up:",
    "zombie-scripted-held": "READY zombie scripted held set up:",
    "zombie-scripted-ignore": "READY zombie scripted ignore set up:",
    "zombie-scripted-replace": "READY zombie scripted replace set up:",
    "zombie-scripted-replace-child": "READY zombie scripted replace child set up:",
    "zombie-scripted-stress": "READY zombie scripted stress set up:",
}


def identity(path):
    return {"path": str(path), "size": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def matching_sdk_version(engine, flags):
    """The Godot.NET.Sdk version the candidate engine's own build produced.

    The status in the engine's own version string is what GODOT_VERSION_STATUS set for its
    build, and build_assemblies.py turns it into the package version. Deriving it from the
    engine under test is what makes the probe build against that engine instead of whatever
    package version happens to sit in a NuGet cache.
    """
    feed = engine.parent / "GodotSharp" / "Tools" / "nupkgs"
    if not feed.is_dir():
        raise RuntimeError(f"No Godot.NET.Sdk feed next to the candidate engine: {feed}")
    version = subprocess.run(
        [str(engine), "--version"], capture_output=True, text=True, timeout=60, creationflags=flags
    )
    version_text = (version.stdout + version.stderr).strip()
    match = re.search(r"\d+\.\d+\.(.+?)\.mono\.double", version_text)
    if version.returncode != 0 or not match:
        raise RuntimeError(f"Could not read a Mono double version from {engine}: {version_text!r}")
    status = match.group(1)
    prerelease = re.sub(r"(\d+)$", r".\1", status)
    versions = {
        found.group(1)
        for package in feed.glob("Godot.NET.Sdk.*.nupkg")
        if (found := re.fullmatch(r"Godot\.NET\.Sdk\.(.+)\.nupkg", package.name))
        and "-" in found.group(1)
        and found.group(1).split("-", 1)[1] == prerelease
    }
    if len(versions) != 1:
        raise RuntimeError(
            f"The candidate engine reports {status}, which is package version {prerelease}, but {feed} "
            f"has {sorted(versions) or 'no matching package'}. Build the assemblies with the same "
            "GODOT_VERSION_STATUS as the engine; the probe must not fall back to a cached package."
        )
    return versions.pop(), feed


def require_matching_packages(project, packages, feed, version):
    """The restore must have taken the candidate's own packages, not a cached older build."""
    for package_id, file_name in [
        ("godot.net.sdk", f"Godot.NET.Sdk.{version}.nupkg"),
        ("godotsharp", f"GodotSharp.{version}.nupkg"),
    ]:
        restored = packages / package_id / version / f"{package_id}.{version}.nupkg"
        if not restored.is_file():
            raise RuntimeError(f"{file_name} was not restored into {restored}.")
        if identity(restored)["sha256"] != identity(feed / file_name)["sha256"]:
            raise RuntimeError(
                f"{file_name} in {restored} is not the candidate engine's package. "
                "A stale NuGet cache or a different package source was used."
            )
    assets = json.loads((project / ".godot/mono/temp/obj/project.assets.json").read_text(encoding="utf-8"))
    resolved = sorted(name for name in assets["libraries"] if name.startswith("Godot"))
    stale = [name for name in resolved if name.split("/", 1)[1] != version]
    if stale:
        raise RuntimeError(f"The probe resolved packages other than {version}: {stale}.")
    return resolved


def expected_leak_diagnostics(diagnostics, running):
    object_type = "RefCounted" if running else "BoxShape3D"
    types = "(?:RefCounted|CSharpScript)" if running else "BoxShape3D"
    patterns = [
        rf"Leaked instance: {types}:\d+ - Reference count: 1",
        r"Hint: Leaked instances typically happen when nodes are removed from the scene tree.*",
        rf"ERROR: Leaked unsafe reference to object: .*<{object_type}#-?\d+>",
        rf"WARNING: {2 if running else 1} ObjectDB instances? (?:was|were) leaked at exit.*",
    ]
    if not running:
        patterns.append(r"ERROR: 1 RID allocations of type '[^']*GodotShape3D[^']*' were leaked at exit\.")
    return all(any(re.fullmatch(pattern, line) for pattern in patterns) for line in diagnostics)


def cases(version):
    if version == "baseline":
        return dict(LEGACY)
    if version == "control":
        return {name: [OWNERSHIP[name]] for name in CONTROL}
    selected = dict(LEGACY)
    selected["running-held"] = ["--running-finalizer", "--release-after-quit"]
    selected.update({name: [flag] for name, flag in OWNERSHIP.items()})
    return selected


def case_elapsed_limit(case):
    if case in LONG_GATE_CASES:
        return LONG_GATE_ELAPSED_LIMIT
    return STRESS_CASE_ELAPSED_LIMIT if case == STRESS_CASE else GATED_CASE_ELAPSED_LIMIT


def case_timeout(case):
    if case in LONG_GATE_CASES:
        return LONG_GATE_TIMEOUT
    return STRESS_CASE_TIMEOUT if case == STRESS_CASE else GATED_CASE_TIMEOUT


def judge(version, case, result, elapsed, output, engine):
    assembly_lines = [line[9:] for line in output.splitlines() if line.startswith("ASSEMBLY ")]
    tolerated = case in TOLERATED_WARNING and output.count(TOLERATED_WARNING[case]) <= TOLERATED_WARNING_LIMIT
    diagnostics = [
        line
        for line in output.splitlines()
        if any(token in line.lower() for token in ["error:", "warning:", "leaked", "fail ", "crashed", "fatal:"])
        and not (case in EXPECTED_WARNING and EXPECTED_WARNING[case] in line)
        and not (tolerated and TOLERATED_WARNING[case] in line)
    ]
    owned = case in OWNERSHIP
    running = case.startswith("running")
    drained = case.endswith("drained")
    passed = elapsed < case_elapsed_limit(case) and len(assembly_lines) == 1
    passed &= Path(assembly_lines[0]).resolve().is_relative_to(engine.parent) if assembly_lines else False
    if owned:
        ready = READY.get(case, "") in output
        warned = EXPECTED_WARNING.get(case, "") in output
        success = result.returncode == 0 and ready and warned and f"PASS {case}:" in output and not diagnostics
        if version == "control":
            failed = f"FAIL {case}:" in output or result.returncode != 0
            return passed and failed and f"PASS {case}:" not in output, diagnostics, assembly_lines
        return passed and success, diagnostics, assembly_lines
    ready = "READY running finalizer held before base disposal." if running else "READY queued finalizer held."
    tracking = "TRACKING True" if version != "baseline" else "TRACKING False"
    passed &= result.returncode == 0 and ready in output and tracking in output
    if drained:
        passed &= "CONTROL finalizer queue drained before shutdown." in output and not diagnostics
    elif version == "baseline":
        leaked_type = "RefCounted" if running else "BoxShape3D"
        passed &= f"Leaked instance: {leaked_type}:" in output and "Leaked unsafe reference to object:" in output
        passed &= expected_leak_diagnostics(diagnostics, running)
        passed &= not any(token in output.lower() for token in ["fail ", "crashed", "fatal:"])
    else:
        passed &= not diagnostics
        if running:
            passed &= "PASS running: the finalizer owned the only native pointer transition." in output
    return passed, diagnostics, assembly_lines


def as_text(stream):
    if stream is None:
        return ""
    return stream.decode("utf-8", "replace") if isinstance(stream, bytes) else stream


def warm_up(engine, project, output, flags):
    """One untimed start and quit of the candidate on the probe project, before the timed cases."""
    started = time.monotonic()
    try:
        result = subprocess.run(
            [str(engine), "--headless", "--verbose", "--path", str(project), "--", "--warmup"],
            capture_output=True,
            text=True,
            timeout=WARMUP_TIMEOUT,
            creationflags=flags,
        )
        code, text = result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired as expired:
        code, text = None, as_text(expired.stdout) + as_text(expired.stderr)
    elapsed = time.monotonic() - started
    (output / "warmup.log").write_text(text, encoding="utf-8")
    lowered = text.lower()
    passed = (
        code == 0
        and elapsed < WARMUP_TIMEOUT
        and WARMUP_LINE in text
        and not any(token in lowered for token in ["crashed", "fatal:"])
    )
    return {"exit": code, "elapsed_seconds": elapsed, "passed": passed}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--control", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent
    candidate = args.candidate.resolve()
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    sdk_version, feed = matching_sdk_version(candidate, flags)
    args.output.mkdir(parents=True, exist_ok=False)
    print(f"candidate build package version {sdk_version} from {feed}", flush=True)
    # The probe must use the candidate's own packages, so clear any earlier restore of them.
    packages = project / ".nuget-packages"
    for package_id in GODOT_PACKAGES:
        cached = packages / package_id / sdk_version
        if cached.exists():
            shutil.rmtree(cached)
    config = project / "NuGet.config"
    created_config = not config.exists()
    if created_config:
        sources = (project / "NuGet.sources.xml").read_text(encoding="utf-8")
        config.write_text(
            re.sub(
                r'(<add key="matching-engine" value=")[^"]*(")',
                lambda match: match.group(1) + str(feed) + match.group(2),
                sources,
            ),
            encoding="utf-8",
            newline="\n",
        )
    try:
        build = subprocess.run(
            [
                "dotnet",
                "build",
                str(project / "ShutdownTrackerProbe.csproj"),
                "-c",
                "Debug",
                "-p:MatchingEngineSdkVersion=" + sdk_version,
            ],
            capture_output=True,
            text=True,
            timeout=300,
            creationflags=flags,
            env=dict(os.environ, NUGET_PACKAGES=str(packages)),
        )
    finally:
        if created_config:
            config.unlink()
    (args.output / "build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
    if build.returncode:
        raise RuntimeError("Probe build failed; see build.log.")
    resolved = require_matching_packages(project, packages, feed, sdk_version)
    runs = []
    identities = [
        identity(project / "ShutdownTrackerProbe.cs"),
        identity(project / "FinalizerOverlap.cs"),
        identity(project / ".godot/mono/temp/bin/Debug/ShutdownTrackerProbe.dll"),
    ]
    for package_id, file_name in [
        ("godot.net.sdk", f"Godot.NET.Sdk.{sdk_version}.nupkg"),
        ("godotsharp", f"GodotSharp.{sdk_version}.nupkg"),
    ]:
        identities.append(identity(feed / file_name))
        identities.append(identity(packages / package_id / sdk_version / f"{package_id}.{sdk_version}.nupkg"))
    warmup = warm_up(candidate, project, args.output, flags)
    verdict = "PASS" if warmup["passed"] else "FAIL"
    print(f"candidate-warmup {verdict} ({warmup['elapsed_seconds']:.1f}s, exit {warmup['exit']})", flush=True)
    engines = [("baseline", args.baseline), ("candidate", candidate), ("control", args.control)]
    for version, engine in engines:
        if engine is None:
            continue
        engine = engine.resolve()
        identities.append(identity(engine))
        for case, user_args in cases(version).items():
            name = f"{version}-{case}"
            started = time.monotonic()
            result = subprocess.run(
                [str(engine), "--headless", "--verbose", "--path", str(project), "--", *user_args],
                capture_output=True,
                text=True,
                timeout=case_timeout(case),
                creationflags=flags,
            )
            elapsed = time.monotonic() - started
            output = result.stdout + result.stderr
            (args.output / (name + ".log")).write_text(output, encoding="utf-8")
            passed, diagnostics, assembly_lines = judge(version, case, result, elapsed, output, engine)
            if assembly_lines:
                identities.append(identity(Path(assembly_lines[0])))
            runs.append({
                "name": name,
                "exit": result.returncode,
                "elapsed_seconds": elapsed,
                "passed": bool(passed),
                "diagnostics": diagnostics,
            })
            print(f"{name} {'PASS' if passed else 'FAIL'} ({elapsed:.1f}s)", flush=True)
            if not passed and diagnostics:
                print("  " + " | ".join(diagnostics[:3]), flush=True)
    (args.output / "results.json").write_text(
        json.dumps(
            {
                "matching_sdk_version": sdk_version,
                "matching_sdk_feed": str(feed),
                "resolved_packages": resolved,
                "nuget_packages": str(packages),
                "identities": identities,
                "warmup": warmup,
                "runs": runs,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    if not warmup["passed"]:
        raise RuntimeError("Shutdown tracker warm-up failed; see warmup.log and results.json.")
    if not all(run["passed"] for run in runs):
        raise RuntimeError("Shutdown tracker comparison failed; see results.json.")


if __name__ == "__main__":
    main()
