import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def identity(path):
    return {'path': str(path), 'size': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def expected_leak_diagnostics(diagnostics, running):
    object_type = 'RefCounted' if running else 'BoxShape3D'
    types = '(?:RefCounted|CSharpScript)' if running else 'BoxShape3D'
    patterns = [rf'Leaked instance: {types}:\d+ - Reference count: 1',
                r'Hint: Leaked instances typically happen when nodes are removed from the scene tree.*',
                rf'ERROR: Leaked unsafe reference to object: .*<{object_type}#-?\d+>',
                rf'WARNING: {2 if running else 1} ObjectDB instances? (?:was|were) leaked at exit.*']
    if not running:
        patterns.append(r"ERROR: 1 RID allocations of type '[^']*GodotShape3D[^']*' were leaked at exit\.")
    return all(any(re.fullmatch(pattern, line) for pattern in patterns) for line in diagnostics)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent
    args.output.mkdir(parents=True, exist_ok=False)
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    build = subprocess.run(['dotnet', 'build', str(project / 'ShutdownTrackerProbe.csproj'), '-c', 'Debug'],
                           capture_output=True, text=True, timeout=90, creationflags=flags)
    (args.output / 'build.log').write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        raise RuntimeError('Probe build failed; see build.log.')
    runs = []
    identities = [identity(project / 'ShutdownTrackerProbe.cs'),
                  identity(project / '.godot/mono/temp/bin/Debug/ShutdownTrackerProbe.dll')]
    for version, engine in [('baseline', args.baseline.resolve()), ('candidate', args.candidate.resolve())]:
        identities.append(identity(engine))
        for running in [False, True]:
            for drained in [False, True]:
                name = f'{version}-{"running" if running else "queued"}-{"drained" if drained else "held"}'
                user_args = (['--running-finalizer'] if running else []) + (['--release-before-quit'] if drained else [])
                started = time.monotonic()
                result = subprocess.run([str(engine), '--headless', '--verbose', '--path', str(project), '--', *user_args],
                                        capture_output=True, text=True, timeout=40, creationflags=flags)
                elapsed = time.monotonic() - started
                output = result.stdout + result.stderr
                (args.output / (name + '.log')).write_text(output, encoding='utf-8')
                assembly_lines = [line[9:] for line in output.splitlines() if line.startswith('ASSEMBLY ')]
                ready = 'READY running finalizer held before base disposal.' if running else 'READY queued finalizer held.'
                tracking = 'TRACKING True' if version == 'candidate' else 'TRACKING False'
                diagnostics = [line for line in output.splitlines() if any(token in line.lower()
                               for token in ['error:', 'warning:', 'leaked', 'fail ', 'crashed', 'fatal:'])]
                passed = result.returncode == 0 and ready in output and tracking in output and elapsed < 15
                passed &= len(assembly_lines) == 1 and Path(assembly_lines[0]).resolve().is_relative_to(engine.parent)
                if assembly_lines:
                    identities.append(identity(Path(assembly_lines[0])))
                if drained:
                    passed &= 'CONTROL finalizer queue drained before shutdown.' in output and not diagnostics
                elif version == 'baseline':
                    leaked_type = 'RefCounted' if running else 'BoxShape3D'
                    passed &= f'Leaked instance: {leaked_type}:' in output and 'Leaked unsafe reference to object:' in output
                    passed &= expected_leak_diagnostics(diagnostics, running)
                    passed &= not any(token in output.lower() for token in ['fail ', 'crashed', 'fatal:'])
                else:
                    passed &= not diagnostics
                    if running:
                        passed &= 'PASS shutdown native pointer transition once; finalizer completed.' in output
                runs.append({'name': name, 'exit': result.returncode, 'elapsed_seconds': elapsed,
                             'passed': bool(passed), 'diagnostics': diagnostics})
                print(name, 'PASS' if passed else 'FAIL', flush=True)
    (args.output / 'results.json').write_text(json.dumps({'identities': identities, 'runs': runs}, indent=2), encoding='utf-8')
    if not all(run['passed'] for run in runs):
        raise RuntimeError('Shutdown tracker comparison failed; see results.json.')


if __name__ == '__main__':
    main()
