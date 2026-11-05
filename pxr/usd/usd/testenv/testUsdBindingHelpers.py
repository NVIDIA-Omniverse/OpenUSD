#!/pxrpythonsubst
# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Validate Usd's binding surface and shared downstream edit-context carrier."""
import argparse
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'base/tf/testenv'))
from testTfCoreDependencyBoundary import run

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source-dir', 'build-dir', 'prefix', 'cmake', 'generator', 'config',
                 'ownership', 'install-manifest'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    prefix, build = Path(args.prefix).resolve(), Path(args.build_dir).resolve()
    ownership = dict(line.split('\t', 1) for line in Path(args.ownership).read_text().splitlines())
    helpers = {'pyConversions.h', 'pyEditContext.h'}
    if any(h.startswith('boost/') or h in helpers for h in ownership['headers'].split(';')):
        raise RuntimeError('Binding header remains in the core public header surface')
    if 'pyConversionRuntime.h' not in ownership['headers'].split(';'):
        raise RuntimeError('The neutral runtime header must remain core-owned')
    if any(Path(p).stem == 'pyEditContext' for p in ownership['sources'].split(';')):
        raise RuntimeError('Core still compiles the edit-context carrier')
    if 'usdBoostPython' in ownership['links'].split(';'):
        raise RuntimeError('Module-only dependencies leaked into core Usd')
    if 'module_links' in ownership and 'usdBoostPython' not in ownership['module_links'].split(';'):
        raise RuntimeError('pxr_module did not supply the binding requirement')
    manifest = {Path(p).as_posix() for p in Path(args.install_manifest).read_text().splitlines()}
    for helper in helpers:
        old = prefix / 'include/pxr/usd/usd' / helper
        new = old.parent / 'boost' / helper
        if old.as_posix() in manifest or old.exists():
            raise RuntimeError('Legacy binding header remains installed: ' + str(old))
        if new.as_posix() not in manifest:
            raise RuntimeError('Missing installed binding header: ' + str(new))
    run([args.cmake, '-S', args.source_dir, '-B', build, '-G', args.generator,
         f'-DUSD_PREFIX={prefix}'])
    output = run([args.cmake, '--build', build, '--config', args.config, '--clean-first',
                  '--target', 'contextHeaderLimited', '--parallel', '2'],
                 log_path=build / 'carrier-build.log')
    # The carrier is itself owned under usd/boost/, but must not pull in any
    # other binding adapter or Boost implementation header.
    includes = [line.replace('\\', '/') for line in output.splitlines()]
    if any(re.search(r'/boost/.*[.](h|hpp)', line, re.I) and
           not re.search(r'/usd/boost/pyEditContext[.]h\s*$', line, re.I)
           for line in includes):
        raise RuntimeError('The standalone Limited API carrier includes Boost')
    run([args.cmake, '--build', build, '--config', args.config, '--parallel', '2'],
        log_path=build / 'build.log')
    module = Path((build / f'module-{args.config}.txt').read_text().strip())
    runtime = (build / f'runtime-{args.config}.txt').read_text().strip()
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(prefix / 'lib'), str(prefix / 'bin'), runtime,
                                  sys.base_prefix, env.get('PATH', '')])
    env['PYTHONPATH'] = os.pathsep.join([str(prefix / 'lib/python'), str(module.parent)])
    print(run([sys.executable, Path(args.source_dir).resolve() / 'validate.py', prefix, runtime], env=env, timeout=60))
    if sys.platform == 'win32':
        dumpbin = Path((build / 'linker.txt').read_text().strip()).with_name('dumpbin.exe')
        exports = run([dumpbin, '/EXPORTS', (build / f'core-{args.config}.txt').read_text().strip()])
        (build / 'core-exports.log').write_text(exports)
        if 'UsdPyEditContext' in exports:
            raise RuntimeError('Core still exports binding-carrier constructors')
    print('PASS: interface ownership, module-only requirements and downstream binding interoperability')

if __name__ == '__main__':
    main()
