#!/pxrpythonsubst
# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Check the shared neutral registry and downstream spec binding adapters."""
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
    build, prefix = Path(args.build_dir).resolve(), Path(args.prefix).resolve()
    run([args.cmake, '-S', args.source_dir, '-B', build, '-G', args.generator,
         f'-DUSD_PREFIX={prefix}'])
    native_targets = ['holderNative', 'holderContracts', 'registryHeaderFull',
        'registryHeaderLimited', 'registrySourceLimited', 'sdfSpecRegister', 'sdfSpecConsume']
    output = run([args.cmake, '--build', build, '--config', args.config,
                  '--clean-first', '--target', *native_targets, '--parallel', '2'],
                 log_path=build / 'native-build.log')
    if any(re.search(r'/boost/.*[.](h|hpp)', line.replace('\\', '/'), re.I)
           for line in output.splitlines()):
        raise RuntimeError('Core spec registry/consumer includes Boost; see native-build.log')
    run([args.cmake, '--build', build, '--config', args.config, '--parallel', '2'],
        log_path=build / 'bindings-build.log')
    runtime = (build / f'runtime-{args.config}.txt').read_text().strip()
    native = Path((build / f'native-{args.config}.txt').read_text().strip())
    module = Path((build / f'module-{args.config}.txt').read_text().strip())
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(native.parent), str(prefix / 'lib'),
        str(prefix / 'bin'), runtime, sys.base_prefix, env.get('PATH', '')])
    env['PYTHONPATH'] = os.pathsep.join([str(prefix / 'lib/python'), str(module.parent)])
    contracts = (build / f'contracts-{args.config}.txt').read_text().strip()
    print(run([contracts], env=env, timeout=60))
    for mode in ('raw', 'binding'):
        executable = contracts if mode == 'raw' else sys.executable
        print(run([executable, Path(args.source_dir).resolve() / 'validate.py',
                   build, args.config, prefix, mode], env=env, timeout=60))
    if sys.platform == 'win32':
        dumpbin = Path((build / 'linker.txt').read_text().strip()).with_name('dumpbin.exe')
        core = (build / f'core-{args.config}.txt').read_text().strip()
        exports = run([dumpbin, '/EXPORTS', core])
        (build / 'registry-exports.log').write_text(exports)
        if any('Sdf_PySpecDetail' in line and
               ('_DummyInit' in line or '_SpecRepr' in line)
               for line in exports.splitlines()):
            raise RuntimeError('Core Sdf still exports Boost spec helpers')

if __name__ == '__main__':
    main()
