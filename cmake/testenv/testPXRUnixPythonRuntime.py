# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Exercise Unix runtime/discovery policy using a Python discovery double.

This runs on every host. It does not substitute for Linux/macOS binary tests;
testTfPackageComponents supplies the real installed-runtime integration checks.
"""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ('source', 'build', 'cmake', 'generator'):
    parser.add_argument('--' + name, required=True)
args = parser.parse_args()
build = Path(args.build)
build.mkdir(parents=True, exist_ok=True)
cases = [
    ('runtime-linux', [], None),
    ('runtime-darwin', ['-DSIMULATE_APPLE=ON'], None),
    ('disabled', ['-DPXR_ENABLE_PYTHON_SUPPORT=OFF'], None),
    ('invalid-native', ['-DINVALID_NATIVE_DEPENDENCY=ON'], 'reaches binding/full API'),
    ('core-linux-313', ['-DPACKAGE_COMPONENT=tf', '-DMOCK_PYTHON_MINOR=13'], None),
    ('core-darwin-313', ['-DPACKAGE_COMPONENT=tf', '-DMOCK_PYTHON_MINOR=13', '-DSIMULATE_APPLE=ON'], None),
    ('bindings-313', ['-DPACKAGE_COMPONENT=tfBoostPython', '-DMOCK_PYTHON_MINOR=13'], '3.12'),
    ('default-313', ['-DPACKAGE_COMPONENT=default', '-DMOCK_PYTHON_MINOR=13'], '3.12'),
]
for name, options, error in cases:
    command = [args.cmake, '-S', str(Path(args.source) / 'cmake/testenv/testPXRUnixPythonRuntime'),
        '-B', str(build / name), '-G', args.generator, '-DUSD_SOURCE=' + args.source,
        '-DPXR_ENABLE_PYTHON_SUPPORT=ON', '-DMOCK_PYTHON_MINOR=12', *options]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (build / (name + '.log')).write_text(result.stdout)
    if (result.returncode == 0) != (error is None) or (error and error not in result.stdout):
        raise RuntimeError(name + ':\n' + result.stdout)
print('PASS: Unix native/embedding separation, disabled Python, dependency rejection and core/binding discovery policy')
