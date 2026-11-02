#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Validate binding-neutral value templates against an installed SDK."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tf/testenv"))
from testTfCoreDependencyBoundary import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-dir", "build-dir", "prefix", "cmake", "generator", "config"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    build = Path(args.build_dir).resolve()
    prefix = Path(args.prefix).resolve()
    run([args.cmake, "-S", args.source_dir, "-B", build, "-G", args.generator,
         f"-DUSD_PREFIX={prefix}"])
    output = run([args.cmake, "--build", build, "--config", args.config,
                  "--clean-first", "--target", "vtNativeTypes", "valueContracts",
                  "valueHeader_full", "valueHeader_limited", "valueSourcesLimited",
                  "--parallel", "2"],
                 log_path=build / "native-build.log")
    if any("/boost/" in line.replace(chr(92), "/").lower()
           for line in output.splitlines()):
        raise RuntimeError("Native templates include Boost; see native-build.log")
    run([args.cmake, "--build", build, "--config", args.config, "--parallel", "2"],
        log_path=build / "bindings-build.log")
    rejected = subprocess.run(
        [args.cmake, "--build", build, "--config", args.config,
         "--target", "missingAdapter"], capture_output=True, text=True)
    (build / "missing-adapter.log").write_text(rejected.stdout + rejected.stderr)
    if (not rejected.returncode or "Include the binding's Vt value adapter" not in
            rejected.stdout + rejected.stderr):
        raise RuntimeError("Missing binding adapter did not fail with the storage diagnostic")

    dependency_dir = (build / f"runtime-{args.config}.txt").read_text().strip()
    native = Path((build / f"native-{args.config}.txt").read_text().strip())
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(native.parent), str(prefix / "lib"),
                                  str(prefix / "bin"), dependency_dir,
                                  sys.base_prefix, env.get("PATH", "")])
    contracts = Path((build / f"contracts-{args.config}.txt").read_text().strip())
    run([contracts], env=env, timeout=60)
    module = Path((build / f"module-{args.config}.txt").read_text().strip())
    env["PYTHONPATH"] = os.pathsep.join([str(prefix / "lib/python"), str(module.parent)])
    run([sys.executable, str(Path(args.source_dir) / "validate.py")], env=env, timeout=60)
    if sys.platform == "win32":
        dumpbin = Path((build / "linker.txt").read_text().strip()).with_name("dumpbin.exe")
        imports = run([dumpbin, "/DEPENDENTS", native])
        (build / "native-imports.log").write_text(imports)
        binding_name = (build / f"binding-{args.config}.txt").read_text().strip()
        if ((binding_name and binding_name.lower() in imports.lower()) or
                re.search(r"python3[0-9]+(?:_d)?[.]dll", imports, re.I)):
            raise RuntimeError("Native value templates import a binding/versioned Python runtime")
        core = Path((build / f"core-{args.config}.txt").read_text().strip())
        exports = run([dumpbin, "/EXPORTS", core])
        (build / "core-exports.log").write_text(exports)
        if "arg_rvalue_from_python" in exports and "VtValueRef" in exports:
            if any("arg_rvalue_from_python" in line and "VtValueRef" in line
                   for line in exports.splitlines()):
                raise RuntimeError("Vt still exports the Boost VtValueRef argument converter")
    print("PASS: native templates, full/Limited API headers, module conversions and storage boundary")


if __name__ == "__main__":
    main()
