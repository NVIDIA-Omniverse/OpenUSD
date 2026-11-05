#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Check stable core discovery, embedding and reuse across Python versions."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from testTfCoreDependencyBoundary import run


def python_info(executable):
    return json.loads(run([executable, "-c", "import json,sys,sysconfig; "
        "print(json.dumps(dict(executable=sys.executable, root=sys.base_prefix, "
        "include=sysconfig.get_path('include'), minor=sys.version_info.minor)))"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("prefix", "source-dir", "build-dir", "cmake", "generator", "config"):
        parser.add_argument(f"--{name}", required=True)
    parser.add_argument("--alternate-python")
    parser.add_argument("--core-component", default="tf")
    parser.add_argument("--binding-component", default="tfBoostPython")
    args = parser.parse_args()
    prefix = Path(args.prefix)
    build = Path(args.build_dir)
    build.mkdir(parents=True, exist_ok=True)
    original = python_info(sys.executable)
    alternate = python_info(args.alternate_python) if args.alternate_python else None

    # Audit only Python metadata; other dependency export behavior is unchanged.
    exported = [prefix / "pxrConfig.cmake", *sorted((prefix / "cmake").glob("pxrTargets*.cmake"))]
    forbidden = [original["root"], original["executable"]]
    if alternate:
        forbidden.extend((alternate["root"], alternate["executable"]))
    for path in exported:
        text = path.read_text().replace("\\", "/").lower()
        for location in forbidden:
            if location.replace("\\", "/").lower() in text:
                raise RuntimeError(f"Build-time Python path in {path}: {location}")

    def configure(name, component, info, *, prefind=False, succeeds=True):
        root = Path(info["root"])
        case = build / name
        command = [args.cmake, "-S", args.source_dir, "-B", case, "-G", args.generator,
            f"-DUSD_PREFIX={prefix.as_posix()}", f"-DPACKAGE_COMPONENT={component}",
            f"-DPYTHON_MINOR={info['minor']}", f"-DPREFIND_PYTHON={'ON' if prefind else 'OFF'}",
            f"-DPython3_EXECUTABLE={Path(info['executable']).as_posix()}",
            f"-DPython3_ROOT_DIR={root.as_posix()}",
            f"-DPython3_INCLUDE_DIR={Path(info['include']).as_posix()}"]
        if sys.platform == 'win32':
            command += [
                f"-DPython3_LIBRARY={(root / ('libs/python3' + str(info['minor']) + '.lib')).as_posix()}",
                f"-DPython3_SABI_LIBRARY={(root / 'libs/python3.lib').as_posix()}"]
        result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace")
        (build / (name + ".log")).write_text(result.stdout)
        if (result.returncode == 0) != succeeds:
            raise RuntimeError(f"Unexpected {name} result:\n{result.stdout}")
        if not succeeds:
            if component == "unavailable":
                expected = "Unsupported pxr component 'unavailable'"
            else:
                expected = f'exact version "3.{original["minor"]}"'
            if expected not in result.stdout or "Syntax error" in result.stdout:
                raise RuntimeError(f"{name} failed for an unexpected reason:\n{result.stdout}")
            return
        if component == args.core_component:
            run([args.cmake, "--build", case, "--config", args.config, "--parallel", "2"],
                log_path=build / (name + "-build.log"))
            consumers = (case / f"consumer-{args.config}.txt").read_text().splitlines()
            env = os.environ.copy()
            env["PYTHONHOME"] = str(root)
            env["PYTHONPATH"] = ""
            env["PYTHONNOUSERSITE"] = "1"
            env["PATH"] = os.pathsep.join([
                str(root), str(prefix / "lib"), str(prefix / "bin"), env.get("PATH", "")])
            for index, consumer in enumerate(consumers):
                output = run([consumer], env=env, timeout=60,
                             log_path=build / (name + f"-run-{index}.log"))
                if not output.startswith(f"3.{info['minor']}."):
                    raise RuntimeError(f"Unexpected embedded Python runtime: {output}")
                if sys.platform == "win32":
                    dumpbin = Path((case / 'linker.txt').read_text()).with_name('dumpbin.exe')
                    imports = run([dumpbin, '/DEPENDENTS', consumer])
                    if 'python3.dll' not in imports.lower() or f'python3{info["minor"]}.dll' in imports.lower():
                        raise RuntimeError('Core consumer did not retain stable Python linkage: ' + imports)

    def probe_runtime(info, probe, name):
        # PyDLL keeps the GIL held while the native probe uses the C API.
        script = (
            'import ctypes,os,sys; from pathlib import Path; '
            'dirs=[Path(sys.argv[2])/"lib",Path(sys.argv[2])/"bin",Path(sys.base_prefix)]; '
            'handles=[os.add_dll_directory(str(p)) for p in dirs if p.is_dir()] '
            'if os.name=="nt" else []; '
            'library=ctypes.PyDLL(sys.argv[1]); '
            'library.ProbePythonVersion.restype=ctypes.c_char_p; '
            'assert library.ProbePythonVersion().decode().split()[0]==sys.version.split()[0]; '
            'assert library.ProbeAnswer()==42; '
            'print(sys.version)')
        env = os.environ.copy()
        env.pop('PYTHONHOME', None)
        env.pop('PYTHONPATH', None)
        run([info['executable'], '-c', script, probe, prefix], env=env,
            log_path=build / (name + '-probe.log'), timeout=60)

    configure("core", args.core_component, original)
    core_case = build / 'core'
    probe = Path((core_case / f'probe-{args.config}.txt').read_text().strip())
    core = Path((core_case / f'core-{args.config}.txt').read_text().strip())
    before = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in (probe, core)}
    if sys.platform != 'win32':
        for name, path in (('core', core), ('probe', probe)):
            command = ['otool', '-L', path] if sys.platform == 'darwin' else ['readelf', '-d', path]
            imports = run(command, log_path=build / (name + '-imports.log'))
            if any(re.search(r'libpython|Python\.framework', line, re.I) for line in imports.splitlines()
                   if sys.platform == 'darwin' or '(NEEDED)' in line):
                raise RuntimeError('Native library records a Python runtime dependency: ' + imports)
    probe_runtime(original, probe, 'original')
    configure("bindings", args.binding_component, original)
    configure("default", "default", original)
    configure("unknown", "unavailable", original, succeeds=False)
    configure("optional", "optional", original)
    if alternate:
        if alternate["minor"] <= original["minor"]:
            raise RuntimeError("Alternate Python must be newer than the package's original Python")
        configure("core-alternate", args.core_component, alternate)
        probe_runtime(alternate, probe, 'alternate')
        configure("bindings-alternate", args.binding_component, alternate, succeeds=False)
        configure("bindings-prefound-alternate", args.binding_component, alternate,
                  prefind=True, succeeds=False)
        configure("default-alternate", "default", alternate, succeeds=False)
    if any(hashlib.sha256(path.read_bytes()).hexdigest() != digest
           for path, digest in before.items()):
        raise RuntimeError('Core or native probe changed during Python version reuse checks')
    print(f"PASS: stable {args.core_component} core and versioned binding package components;")
    print("no build-time Python paths in installed config or target exports.")


if __name__ == "__main__":
    main()
