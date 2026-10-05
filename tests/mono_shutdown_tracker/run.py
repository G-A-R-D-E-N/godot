import argparse
import hashlib
import json
import os
import re
import subprocess
import time
from pathlib import Path

LEGACY = {
    "queued-held": [],
    "queued-drained": ["--release-before-quit"],
    "running-held": ["--running-finalizer"],
    "running-drained": ["--running-finalizer", "--release-before-quit"],
}
OWNERSHIP = {"overlap": "--overlap-finalizer", "prebase": "--prebase-finalizer", "race": "--race-dispose"}
READY = {
    "overlap": "READY overlap finalizer held inside base disposal.",
    "prebase": "READY prebase finalizer held before base disposal.",
}


def identity(path):
    return {"path": str(path), "size": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


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
        return {name: [OWNERSHIP[name]] for name in ["overlap", "prebase"]}
    selected = dict(LEGACY)
    selected["running-held"] = ["--running-finalizer", "--release-after-quit"]
    selected.update({name: [flag] for name, flag in OWNERSHIP.items()})
    return selected


def judge(version, case, result, elapsed, output, engine):
    assembly_lines = [line[9:] for line in output.splitlines() if line.startswith("ASSEMBLY ")]
    diagnostics = [
        line
        for line in output.splitlines()
        if any(token in line.lower() for token in ["error:", "warning:", "leaked", "fail ", "crashed", "fatal:"])
    ]
    owned = case in OWNERSHIP
    running = case.startswith("running")
    drained = case.endswith("drained")
    passed = elapsed < 15 and len(assembly_lines) == 1
    passed &= Path(assembly_lines[0]).resolve().is_relative_to(engine.parent) if assembly_lines else False
    if owned:
        ready = READY.get(case, "") in output or case == "race"
        success = result.returncode == 0 and ready and f"PASS {case}:" in output and not diagnostics
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--control", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent
    args.output.mkdir(parents=True, exist_ok=False)
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    config = project / "NuGet.config"
    created_config = not config.exists()
    if created_config:
        config.write_bytes((project / "NuGet.sources.xml").read_bytes())
    try:
        build = subprocess.run(
            ["dotnet", "build", str(project / "ShutdownTrackerProbe.csproj"), "-c", "Debug"],
            capture_output=True,
            text=True,
            timeout=300,
            creationflags=flags,
        )
    finally:
        if created_config:
            config.unlink()
    (args.output / "build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
    if build.returncode:
        raise RuntimeError("Probe build failed; see build.log.")
    runs = []
    identities = [
        identity(project / "ShutdownTrackerProbe.cs"),
        identity(project / "FinalizerOverlap.cs"),
        identity(project / ".godot/mono/temp/bin/Debug/ShutdownTrackerProbe.dll"),
    ]
    engines = [("baseline", args.baseline), ("candidate", args.candidate), ("control", args.control)]
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
                timeout=40,
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
            print(name, "PASS" if passed else "FAIL", flush=True)
    (args.output / "results.json").write_text(
        json.dumps({"identities": identities, "runs": runs}, indent=2), encoding="utf-8"
    )
    if not all(run["passed"] for run in runs):
        raise RuntimeError("Shutdown tracker comparison failed; see results.json.")


if __name__ == "__main__":
    main()
