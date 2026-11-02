#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Build external registry consumers against the installed SDK."""
import argparse
import os
from pathlib import Path
import re
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
                  "--clean-first", "--target", "registryHeader_full",
                  "registryHeader_limited", "--parallel", "2"],
                 log_path=build / "headers.log")
    if any(re.search(r"/boost/.*\.(h|hpp)", line.replace("\\", "/"), re.I)
           for line in output.splitlines()):
        raise RuntimeError("Neutral registry header includes Boost; see headers.log")
    run([args.cmake, "--build", build, "--config", args.config,
         "--parallel", "2"], log_path=build / "build.log")
    env = os.environ.copy()
    dependency_dir = (build / f"runtime-{args.config}.txt").read_text().strip()
    env["PATH"] = os.pathsep.join([str(prefix / "lib"), str(prefix / "bin"),
                                  dependency_dir, sys.base_prefix,
                                  env.get("PATH", "")])
    native = Path((build / f"contracts-{args.config}.txt").read_text().strip())
    run([native], env=env, timeout=60)
    module = Path((build / f"module-{args.config}.txt").read_text().strip())
    env["PYTHONPATH"] = os.pathsep.join([str(prefix / "lib/python"),
                                       str(module.parent)])
    run([sys.executable, str(Path(args.source_dir) / "validate.py")],
        env=env, timeout=60)
    print("PASS: neutral header compiles with full and Limited API; native and module contracts")


if __name__ == "__main__":
    main()
