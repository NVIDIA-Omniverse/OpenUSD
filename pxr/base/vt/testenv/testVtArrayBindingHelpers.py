#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Verify installed array helpers and module-private buffer ownership."""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tf/testenv"))
from testTfCoreDependencyBoundary import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-dir", "build-dir", "prefix", "cmake", "generator", "config",
                 "ownership", "install-manifest"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    build = Path(args.build_dir).resolve()
    prefix = Path(args.prefix).resolve()
    manifest = Path(args.install_manifest).read_text().replace("\\", "/")
    for name in ("arrayPyBuffer.h", "wrapArray.h", "wrapArrayEdit.h", "pyOperators.h"):
        path = f"pxr/base/vt/{name}"
        if (prefix / "include" / path).exists() or f"/include/{path}" in manifest:
            raise RuntimeError(f"Private/moved core header still installed: {path}")
    ownership = dict(line.split("\t", 1) for line in
                     Path(args.ownership).read_text().splitlines())
    for source in ("arrayPyBuffer.cpp", "wrapArray.cpp"):
        if source in ownership["core"]:
            raise RuntimeError(f"Binding implementation remains in core: {source}")
    if "arrayPyBuffer.cpp" not in ownership["module"]:
        raise RuntimeError("Buffer implementation is not owned by the Python module")
    run([args.cmake, "-S", args.source_dir, "-B", build, "-G", args.generator,
         f"-DUSD_PREFIX={prefix}"])
    run([args.cmake, "--build", build, "--config", args.config,
         "--clean-first", "--parallel", "2"], log_path=build / "build.log")
    module = Path((build / f"module-{args.config}.txt").read_text().strip())
    dependency_dir = (build / f"runtime-{args.config}.txt").read_text().strip()
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(prefix / "lib"), str(prefix / "bin"),
                                  dependency_dir, sys.base_prefix, env.get("PATH", "")])
    env["PYTHONPATH"] = os.pathsep.join([str(prefix / "lib/python"), str(module.parent)])
    run([sys.executable, str(Path(args.source_dir) / "validate.py")], env=env, timeout=60)
    if sys.platform == "win32":
        dumpbin = Path((build / "linker.txt").read_text().strip()).with_name("dumpbin.exe")
        forbidden = ("GetVtArrayName", "Vt_ComputeEffectiveRankAndLastDimSize",
                     "VtArrayFromPyBuffer", "Vt_AddBufferProtocolSupportToVtArrays")
        for binary in (Path(ownership["core_file"]), Path(ownership["module_file"])):
            exports = run([dumpbin, "/EXPORTS", binary])
            (build / f"{binary.stem}-exports.log").write_text(exports)
            if any(name in exports for name in forbidden):
                raise RuntimeError(f"Array/buffer binding helper exported from {binary}")
        binding = (build / f"binding-{args.config}.txt").read_text().strip()
        if binding:
            imports = run([dumpbin, "/DEPENDENTS", ownership["core_file"]])
            (build / "core-imports.log").write_text(imports)
            if binding.lower() in imports.lower():
                raise RuntimeError("Core Vt still imports the Boost.Python runtime")
    print("PASS: binding target ownership, downstream helpers and private module buffers")


if __name__ == "__main__":
    main()
