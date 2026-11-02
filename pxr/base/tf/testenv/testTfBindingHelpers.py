#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Build and run an external module using only the Tf binding interface."""

import argparse
import os
from pathlib import Path
import sys

from testTfCoreDependencyBoundary import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-dir", "build-dir", "prefix", "headers", "cmake",
                 "generator", "config"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    build = Path(args.build_dir)
    prefix = Path(args.prefix)

    run([args.cmake, "-S", args.source_dir, "-B", build,
         "-G", args.generator, f"-DUSD_PREFIX={prefix}",
         f"-DBINDING_HEADERS={args.headers}"])
    run([args.cmake, "--build", build, "--config", args.config,
         "--clean-first", "--parallel", "2"], log_path=build / "build.log")

    module = Path((build / f"module-{args.config}.txt").read_text().strip())
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([
        str(prefix / "lib"), str(prefix / "bin"), str(Path(sys.base_prefix)),
        env.get("PATH", "")])
    env["PYTHONPATH"] = os.pathsep.join([
        str(prefix / "lib/python"), str(module.parent), env.get("PYTHONPATH", "")])
    script = """
from pxr import Tf
import tfBindingConsumer as module
assert module.values() == [1, 2, 3]
try:
    module.raise_error()
except Tf.ErrorException as error:
    assert 'expected binding-helper error' in str(error)
else:
    raise AssertionError('TfPyRaiseOnError did not translate the diagnostic')
"""
    run([sys.executable, "-c", script], env=env, timeout=60)
    count = len(Path(args.headers).read_text().splitlines())
    print(f"PASS: {count} binding helpers compiled; external module linked only")
    print("tfBoostPython; sequence conversion and error translation passed.")


if __name__ == "__main__":
    main()
