# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Exercise explicit native/module ownership through the real CMake macros."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'prefix', 'cmake', 'generator', 'config'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    source, build = Path(args.source), Path(args.build)
    project = build / 'project'
    shutil.copytree(source / 'cmake/testenv/testPXRModuleSplit', project, dirs_exist_ok=True)
    # The existing build helpers locate installation/generation scripts through
    # PROJECT_SOURCE_DIR. Give this isolated project their real implementations.
    shutil.copytree(source / 'cmake', project / 'cmake', dirs_exist_ok=True)

    def run(command, name, succeeds=True, **kwargs):
        result = subprocess.run(list(map(str, command)), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
        (build / (name + '.log')).write_text(result.stdout)
        if (result.returncode == 0) != succeeds:
            raise RuntimeError(result.stdout)
        return result.stdout

    for name, enabled, monolithic in (('enabled', True, False),
                                      ('disabled', False, False),
                                      ('monolithic', True, True)):
        case = build / name
        prefix = case / 'install'
        run([args.cmake, '-S', project, '-B', case, '-G', args.generator,
             f'-DUSD_PREFIX={args.prefix}', f'-DCMAKE_INSTALL_PREFIX={prefix}',
             f'-DPython3_EXECUTABLE={sys.executable}', f'-DPython3_ROOT_DIR={sys.base_prefix}',
             '-DVERIFY_MONOLITHIC_LINKS=' + ('ON' if monolithic else 'OFF'),
             '-DPXR_ENABLE_PYTHON_SUPPORT=' + ('ON' if enabled else 'OFF')],
            'configure-' + case.name)
        run([args.cmake, '--build', case, '--config', args.config,
             '--target', 'install', '--parallel', '2'], 'build-' + case.name)
        native = Path((case / f'native-{args.config}.txt').read_text().strip())
        env = os.environ.copy()
        env['PATH'] = os.pathsep.join([str(native.parent), str(prefix / 'lib'),
            str(Path(args.prefix) / 'lib'), str(Path(args.prefix) / 'bin'),
            sys.base_prefix, env.get('PATH', '')])
        if enabled:
            generated = (case / 'native/moduleDeps.cpp').read_text()
            if 'TfToken("nativeProbe"), TfToken("pxr.BindingProbe")' not in generated:
                raise RuntimeError('Module registration lost the explicit native/module names')
            env['PYTHONPATH'] = os.pathsep.join([str(prefix / 'lib/python'),
                str(Path(args.prefix) / 'lib/python')])
            run([sys.executable, '-c',
                 'import os; from pathlib import Path; '
                 'paths = [Path(p) for p in os.environ["PATH"].split(os.pathsep) if p]; '
                 'handles = [os.add_dll_directory(str(p)) for p in paths if p.is_dir()] '
                 'if os.name == "nt" else []; '
                 'from pxr import BindingProbe, PureProbe, ResourceProbe; '
                 'assert BindingProbe.answer() == 42 and PureProbe.answer == 7 '
                 'and ResourceProbe.answer == 11'],
                'runtime', env=env)
        elif (case / 'native/moduleDeps.cpp').exists() or (prefix / 'lib/python/pxr/BindingProbe').exists():
            raise RuntimeError('Python-disabled build generated/installed a module')
        if not enabled and '/lib/python/' in (case / 'install_manifest.txt').read_text().replace('\\', '/'):
            raise RuntimeError('Python-disabled build installed Python files')
    output = run([args.cmake, '-S', project, '-B', build / 'obsolete', '-G', args.generator,
                 '-DPXR_ENABLE_PYTHON_SUPPORT=OFF', '-DOBSOLETE_ARGUMENTS=ON'],
                'obsolete-arguments', succeeds=False)
    if 'separate pxr_module()' not in output:
        raise RuntimeError(output)
    print('PASS: explicit modules, separate directories/names, pure Python/resources, monolithic links, disabled Python and legacy argument rejection')


if __name__ == '__main__':
    main()
