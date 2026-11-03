#!/pxrpythonsubst
# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Validate Pcp's installed adapter ownership and downstream binding consumers."""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'base/tf/testenv'))
from testTfCoreDependencyBoundary import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source-dir', 'build-dir', 'prefix', 'cmake', 'generator', 'config',
                 'ownership', 'install-manifest'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    build, prefix = Path(args.build_dir).resolve(), Path(args.prefix).resolve()
    ownership = dict(line.split('\t', 1) for line in Path(args.ownership).read_text().splitlines())
    if any(header.startswith('boost/') or header == 'pyUtils.h'
           for header in ownership['headers'].split(';')):
        raise RuntimeError('Binding adapter remains in core public header ownership')
    if any(Path(source).stem == 'pyUtils' for source in ownership['sources'].split(';')):
        raise RuntimeError('Core source list still compiles the binding helper')
    installed = {Path(path).as_posix() for path in Path(args.install_manifest).read_text().splitlines()}
    if str(prefix.as_posix() + '/include/pxr/usd/pcp/pyUtils.h') in installed:
        raise RuntimeError('Legacy flat binding header is still installed')
    if str(prefix.as_posix() + '/include/pxr/usd/pcp/boost/pyUtils.h') not in installed:
        raise RuntimeError('Binding adapter is not installed')
    run([args.cmake, '-S', args.source_dir, '-B', build, '-G', args.generator,
         f'-DUSD_PREFIX={prefix}'])
    run([args.cmake, '--build', build, '--config', args.config, '--parallel', '2'],
        log_path=build / 'build.log')
    module = Path((build / f'module-{args.config}.txt').read_text().strip())
    runtime = (build / f'runtime-{args.config}.txt').read_text().strip()
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(prefix / 'lib'), str(prefix / 'bin'), runtime,
                                  sys.base_prefix, env.get('PATH', '')])
    env['PYTHONPATH'] = os.pathsep.join([str(prefix / 'lib/python'), str(module.parent)])
    print(run([sys.executable, Path(args.source_dir).resolve() / 'validate.py',
               prefix, runtime], env=env, timeout=60))
    if sys.platform == 'win32':
        dumpbin = Path((build / 'linker.txt').read_text().strip()).with_name('dumpbin.exe')
        core = (build / f'core-{args.config}.txt').read_text().strip()
        exports = run([dumpbin, '/EXPORTS', core])
        (build / 'core-exports.log').write_text(exports)
        if 'PcpVariantFallbackMapFromPython' in exports:
            raise RuntimeError('Core still exports the Boost dictionary adapter')
    print('PASS: interface-owned adapter, core ownership and downstream binding consumers')


if __name__ == '__main__':
    main()
