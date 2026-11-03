#!/pxrpythonsubst
# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Audit Usd conversion runtime independently of its default binding module."""
import argparse
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'base/tf/testenv'))
from testTfCoreDependencyBoundary import run

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source-dir', 'build-dir', 'prefix', 'cmake', 'generator', 'config'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    prefix, build = Path(args.prefix).resolve(), Path(args.build_dir).resolve()
    run([args.cmake, '-S', args.source_dir, '-B', build, '-G', args.generator,
         f'-DUSD_PREFIX={prefix}'])
    output = run([args.cmake, '--build', build, '--config', args.config,
                  '--clean-first', '--parallel', '2'], log_path=build / 'build.log')
    if any(re.search(r'/boost/.*[.](h|hpp)', line.replace('\\', '/'), re.I)
           for line in output.splitlines()):
        raise RuntimeError('Native conversion runtime includes Boost; see build.log')
    native = (build / f'native-{args.config}.txt').read_text().strip()
    runtime = (build / f'runtime-{args.config}.txt').read_text().strip()
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(prefix / 'lib'), str(prefix / 'bin'), runtime,
                                  sys.base_prefix, env.get('PATH', '')])
    env['PYTHONPATH'] = str(prefix / 'lib/python')
    for script in ('contracts.py', 'validate.py'):
        print(run([sys.executable, Path(args.source_dir).resolve() / script,
                   prefix, native, runtime], env=env, timeout=60))
    print('PASS: raw runtime header/source/consumer Limited API and Boost-free include traces')

if __name__ == '__main__':
    main()
