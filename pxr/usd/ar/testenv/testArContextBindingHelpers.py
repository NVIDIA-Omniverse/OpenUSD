#!/pxrpythonsubst
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Validate downstream context creation independently of Python bindings."""
import argparse
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "base/tf/testenv"))
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
                  "--clean-first", "--target", "arContextNative", "contextContracts",
                  "contextHeader_full", "contextHeader_limited", "contextSourceLimited",
                  "--parallel", "2"],
                 log_path=build / "native-build.log")
    if any("/boost/" in line.replace(chr(92), "/").lower()
           for line in output.splitlines()):
        raise RuntimeError("Native context includes Boost; see native-build.log")
    run([args.cmake, "--build", build, "--config", args.config, "--parallel", "2"],
        log_path=build / "bindings-build.log")
    dependency_dir = (build / f"runtime-{args.config}.txt").read_text().strip()
    native = Path((build / f"native-{args.config}.txt").read_text().strip())
    module = Path((build / f"module-{args.config}.txt").read_text().strip())
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(native.parent), str(prefix / "lib"),
                                  str(prefix / "bin"), dependency_dir,
                                  sys.base_prefix, env.get("PATH", "")])
    env["PYTHONPATH"] = os.pathsep.join([str(prefix / "lib/python"), str(module.parent)])
    contracts = (build / f"contracts-{args.config}.txt").read_text().strip()
    run([contracts], env=env, timeout=60)
    print(run([sys.executable, str(Path(args.source_dir) / "validate.py"),
               build, args.config, prefix, dependency_dir], env=env, timeout=60))
    if sys.platform == "win32":
        dumpbin = Path((build / "linker.txt").read_text().strip()).with_name("dumpbin.exe")
        imports = run([dumpbin, "/DEPENDENTS", native])
        (build / "native-imports.log").write_text(imports)
        binding_name = (build / f"binding-{args.config}.txt").read_text().strip()
        if ((binding_name and binding_name.lower() in imports.lower()) or
                re.search(r"python3[0-9]+(?:_d)?[.]dll", imports, re.I)):
            raise RuntimeError("Native context directly imports binding/versioned Python")


if __name__ == "__main__":
    main()
