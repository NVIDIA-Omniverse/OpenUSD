#!/pxrpythonsubst
# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Check native Trace's Limited API, tracing contract and dependency boundary."""
import argparse
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tf/testenv'))
from testTfCoreDependencyBoundary import check_no_bundled_python, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source-dir', 'build-dir', 'prefix', 'headers', 'ownership',
                 'install-manifest', 'cmake', 'generator', 'config'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    build, prefix = Path(args.build_dir), Path(args.prefix)
    check_no_bundled_python(args.install_manifest)
    ownership = dict(line.split('\t', 1) for line in Path(args.ownership).read_text().splitlines())
    if 'Py_LIMITED_API=0x030A0000' not in ownership['definitions']:
        raise RuntimeError('Core Trace does not enforce the Limited API')
    if 'Py_LIMITED_API' in ownership['interface_definitions']:
        raise RuntimeError('Trace imposes Limited API restrictions on consumers')
    for field in ('links', 'interface_links'):
        if re.search(r'BoostPython|(^|;)(python|boost)($|;)', ownership[field]) or (
                sys.platform == 'win32' and 'Python3::Python' in ownership[field]):
            raise RuntimeError('Core Trace exposes binding Python: ' + ownership[field])
    run([args.cmake, '-S', args.source_dir, '-B', build, '-G', args.generator,
         f'-DUSD_PREFIX={prefix}', f'-DCORE_HEADERS={args.headers}'])
    output = run([args.cmake, '--build', build, '--config', args.config,
                  '--clean-first', '--parallel', '2'], log_path=build / 'build.log')
    if re.search(r'/boost/.*[.](h|hpp)', output.replace('\\', '/'), re.I):
        raise RuntimeError('Native Trace includes Boost; see build.log')
    consumer = Path((build / f'consumer-{args.config}.txt').read_text().strip())
    runtime = (build / f'runtime-{args.config}.txt').read_text().strip()
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(prefix / 'lib'), str(prefix / 'bin'),
                                  runtime, sys.base_prefix, env.get('PATH', '')])
    run([consumer], env=env, timeout=60)
    if sys.platform == 'win32':
        dumpbin = Path((build / 'linker.txt').read_text().strip()).with_name('dumpbin.exe')
        core = Path((build / f'core-{args.config}.txt').read_text().strip())
        binding = (build / f'binding-{args.config}.txt').read_text().strip()
        for binary in (core, consumer):
            imports = run([dumpbin, '/DEPENDENTS', binary])
            (build / (binary.stem + '-imports.log')).write_text(imports)
            if binding.lower() in imports.lower() or re.search(
                    r'python3\d+(?:_d)?[.]dll', imports, re.I):
                raise RuntimeError('Native Trace imports versioned/binding Python: ' + imports)
            if binary == consumer and 'python3.dll' not in imports.lower():
                raise RuntimeError('The native consumer did not use the stable Python ABI')
    print('PASS: native Trace events, full/Limited headers, collector and core boundary')


if __name__ == '__main__':
    main()
