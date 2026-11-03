#!/pxrpythonsubst
# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Validate native and raw-Python metadata paths through an installed SDK."""
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
    build = Path(args.build_dir).resolve()
    prefix = Path(args.prefix).resolve()
    run([args.cmake, '-S', args.source_dir, '-B', build, '-G', args.generator,
         f'-DUSD_PREFIX={prefix}', '-DCHECK_METADATA_SOURCE=ON'])
    output = run([args.cmake, '--build', build, '--config', args.config,
                  '--clean-first', '--parallel', '2'], log_path=build / 'build.log')
    if any(re.search(r'/boost/.*[.](h|hpp)', line.replace('\\', '/'), re.I)
           for line in output.splitlines()):
        raise RuntimeError('Native metadata source/consumer includes Boost; see build.log')
    runtime = (build / f'runtime-{args.config}.txt').read_text().strip()
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(prefix / 'lib'), str(prefix / 'bin'),
                                 runtime, sys.base_prefix, env.get('PATH', '')])
    env['PYTHONPATH'] = str(prefix / 'lib/python')
    print(run([sys.executable, Path(args.source_dir) / 'validate.py',
               build, args.config, prefix], env=env, timeout=60))

if __name__ == '__main__':
    main()
