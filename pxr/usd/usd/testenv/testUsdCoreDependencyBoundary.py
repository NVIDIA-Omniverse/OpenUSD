#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Validate the installed core Usd surface and its complete Limited API source set."""
import argparse
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "base/tf/testenv"))
from testTfCoreDependencyBoundary import check_no_bundled_python, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-dir", "build-dir", "prefix", "headers", "sources", "ownership",
                 "module-deps", "install-manifest", "cmake", "generator", "config"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    build = Path(args.build_dir).resolve()
    prefix = Path(args.prefix).resolve()
    check_no_bundled_python(args.install_manifest)
    ownership = dict(line.split("\t", 1) for line in
                     Path(args.ownership).read_text().splitlines())
    if "Py_LIMITED_API=0x030A0000" not in ownership["definitions"]:
        raise RuntimeError("Core Usd is not compiled against the Limited API floor")
    if "Py_LIMITED_API" in ownership["interface_definitions"]:
        raise RuntimeError("Limited API restriction leaks into the binding interface")
    forbidden = r"(tf|vt|ar|sdf|pcp|usd)BoostPython|(^|;)(python|boost)($|;)"
    for field in ("links", "interface_links"):
        links = ownership[field]
        if re.search(forbidden, links) or (sys.platform == "win32" and
                re.search(r"Python3::Python|python3[0-9]+[.](lib|dll)", links, re.I)):
            raise RuntimeError(f"Core Usd exposes binding/versioned Python requirements: {links}")
    deps = re.findall(r'TfToken\("([^"]+)"\)', Path(args.module_deps).read_text())
    if not deps or any(d in ("python", "boost", "tfBoostPythonInternal",
                            "tfBoostPython", "vtBoostPython", "arBoostPython",
                            "sdfBoostPython", "pcpBoostPython", "usdBoostPython") or d.startswith("Python3::")
                       for d in deps):
        raise RuntimeError(f"Invalid core module dependencies: {deps}")
    headers = Path(args.headers).read_text().splitlines()
    binding_headers = {"pyConversions.h", "pyEditContext.h"}
    if any(h.startswith("boost/") or h in binding_headers for h in headers):
        raise RuntimeError("Binding helper remains in the core public header surface")

    run([args.cmake, "-S", args.source_dir, "-B", build, "-G", args.generator,
         f"-DUSD_PREFIX={prefix}", f"-DCORE_HEADERS={args.headers}",
         f"-DCORE_SOURCES={args.sources}"])
    output = run([args.cmake, "--build", build, "--config", args.config,
                  "--clean-first", "--parallel", "2"], log_path=build / "build.log")
    if any(re.search(r"/boost/.*[.](h|hpp)", line.replace("\\", "/"), re.I)
           for line in output.splitlines()):
        raise RuntimeError("Core Usd headers or source units include Boost; see build.log")
    consumer = Path((build / f"consumer-{args.config}.txt").read_text().strip())
    dependency_dir = (build / f"runtime-{args.config}.txt").read_text().strip()
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(prefix / "lib"), str(prefix / "bin"),
                                  dependency_dir, sys.base_prefix, env.get("PATH", "")])
    run([consumer], env=env, timeout=60)
    if sys.platform == "win32":
        dumpbin = Path((build / "linker.txt").read_text().strip()).with_name("dumpbin.exe")
        binding = (build / f"binding-{args.config}.txt").read_text().strip()
        core = Path((build / f"core-{args.config}.txt").read_text().strip())
        for binary in (core, consumer):
            imports = run([dumpbin, "/DEPENDENTS", binary])
            (build / f"{binary.stem}-imports.log").write_text(imports)
            if (binding.lower() in imports.lower() or
                    re.search(r"python3[0-9]+(?:_d)?[.]dll", imports, re.I)):
                raise RuntimeError(f"Core consumer imports binding/versioned Python:\n{imports}")
            if binary == consumer and "python3.dll" not in imports.lower():
                raise RuntimeError(f"Core consumer did not link the stable Python ABI:\n{imports}")
    print(f"PASS: {len(headers)} public headers, "
          f"{len(Path(args.sources).read_text().splitlines())} core source units; "
          "Limited API, native runtime and dependency boundary")


if __name__ == "__main__":
    main()
