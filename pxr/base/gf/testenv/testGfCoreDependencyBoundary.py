#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Verify Gf's installed core boundary and private binding helper ownership."""

import argparse
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tf/testenv"))
from testTfCoreDependencyBoundary import check_no_bundled_python, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-dir", "build-dir", "prefix", "headers", "ownership",
                 "module-deps", "install-manifest", "cmake", "generator", "config"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    build = Path(args.build_dir)
    prefix = Path(args.prefix)
    check_no_bundled_python(args.install_manifest)
    private = "pxr/base/gf/pyBufferUtils.h"
    if (prefix / "include" / private).exists():
        raise RuntimeError("Private Gf binding helper is installed")
    if private in Path(args.install_manifest).read_text().replace("\\", "/"):
        raise RuntimeError("Private Gf binding helper is in the install manifest")
    headers = Path(args.headers).read_text().splitlines()
    if "pyBufferUtils.h" in headers:
        raise RuntimeError("Private binding helper remains a core public header")
    ownership = dict(line.split("\t", 1) for line in Path(args.ownership).read_text().splitlines())
    if "pyBufferUtils.cpp" in ownership["core"] or "pyBufferUtils.cpp" not in ownership["module"]:
        raise RuntimeError("Gf buffer helper must compile only into the Python module")
    if re.search(r"Python3::|tfBoostPython|(^|;)(python|boost)($|;)", ownership["links"]):
        raise RuntimeError(f"Direct Gf binding/runtime dependencies: {ownership['links']}")
    dependencies = re.findall(r'TfToken\("([^\"]+)"\)', Path(args.module_deps).read_text())
    if any(d.startswith("Python3::") or d in ("python", "boost", "tfBoostPythonInternal")
           for d in dependencies):
        raise RuntimeError(f"Gf module metadata contains binding/runtime targets: {dependencies}")

    run([args.cmake, "-S", args.source_dir, "-B", build, "-G", args.generator,
         f"-DUSD_PREFIX={prefix}", f"-DCORE_HEADERS={args.headers}"])
    output = run([args.cmake, "--build", build, "--config", args.config,
                  "--clean-first", "--parallel", "2"], log_path=build / "build.log")
    if any(re.search(r"[/\\]boost[/\\].*\.(h|hpp)", line, re.I)
           for line in output.splitlines()):
        raise RuntimeError("Gf core headers pull in Boost headers; see build.log")
    consumer = Path((build / f"consumer-{args.config}.txt").read_text().strip())
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(prefix / "lib"), str(prefix / "bin"),
                                  sys.base_prefix, env.get("PATH", "")])
    run([consumer], env=env, timeout=60)

    if sys.platform == "win32":
        dumpbin = Path((build / "linker.txt").read_text().strip()).with_name("dumpbin.exe")
        core = Path((build / f"core-{args.config}.txt").read_text().strip())
        for binary in (core, consumer):
            imports = run([dumpbin, "/DEPENDENTS", binary])
            (build / f"{binary.stem}-imports.log").write_text(imports)
            if (ownership["binding_file"].lower() in imports.lower()
                    or re.search(r"\bpython\d*(?:_d)?\.dll", imports, re.I)):
                raise RuntimeError(f"Core Gf consumer imports Python or Boost:\n{imports}")
        module = prefix / "lib/python/pxr/Gf" / ownership["module_file"]
        for binary in (core, module):
            exports = run([dumpbin, "/EXPORTS", binary])
            (build / f"{binary.stem}-exports.log").write_text(exports)
            if "Gf_GetPyBufferFmtFor" in exports:
                raise RuntimeError(f"Private buffer helper exported by {binary}")
    print(f"PASS: {len(headers)} Gf public headers compile without Boost;")
    print("core consumer ran; buffer helper is private to the binding module.")


if __name__ == "__main__":
    main()
