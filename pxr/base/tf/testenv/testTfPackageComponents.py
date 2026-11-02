#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Check Python dependency selection in the installed pxr package on Windows."""

import argparse
import json
import os
from pathlib import Path
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
            f"-DPython3_INCLUDE_DIR={Path(info['include']).as_posix()}",
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
            consumer = (case / f"consumer-{args.config}.txt").read_text().strip()
            env = os.environ.copy()
            env["PYTHONHOME"] = str(root)
            env["PYTHONPATH"] = ""
            env["PYTHONNOUSERSITE"] = "1"
            env["PATH"] = os.pathsep.join([
                str(root), str(prefix / "lib"), str(prefix / "bin"), env.get("PATH", "")])
            output = run([consumer], env=env, timeout=60,
                         log_path=build / (name + "-run.log"))
            if not output.startswith(f"3.{info['minor']}."):
                raise RuntimeError(f"Unexpected embedded Python runtime: {output}")

    configure("core", args.core_component, original)
    configure("bindings", args.binding_component, original)
    configure("default", "default", original)
    configure("unknown", "unavailable", original, succeeds=False)
    configure("optional", "optional", original)
    if alternate:
        if alternate["minor"] <= original["minor"]:
            raise RuntimeError("Alternate Python must be newer than the package's original Python")
        configure("core-alternate", args.core_component, alternate)
        configure("bindings-alternate", args.binding_component, alternate, succeeds=False)
        configure("bindings-prefound-alternate", args.binding_component, alternate,
                  prefind=True, succeeds=False)
        configure("default-alternate", "default", alternate, succeeds=False)
    print(f"PASS: stable {args.core_component} core and versioned binding package components;")
    print("no build-time Python paths in installed config or target exports.")


if __name__ == "__main__":
    main()
