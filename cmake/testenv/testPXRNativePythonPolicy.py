# Copyright 2026 Pixar
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
"""Check every declared native/binding target's compilation and link role."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--manifest', required=True)
parser.add_argument('--python-enabled', type=int, required=True)
parser.add_argument('--windows', type=int, required=True)
args = parser.parse_args()
targets = {}
for line in Path(args.manifest).read_text().splitlines():
    name, role, kind, definitions, interface, links, interface_links = line.split('\t')
    targets[name] = dict(role=role, kind=kind, definitions=definitions,
                         interface=interface, links=links + ';' + interface_links)
native = 0
for name, target in targets.items():
    role = target['role']
    if 'Py_LIMITED_API' in target['interface']:
        raise RuntimeError('Native restrictions leak to consumers: ' + name)
    if role == 'NATIVE':
        native += 1
        has_flag = 'Py_LIMITED_API=0x030A0000' in target['definitions'].split(';')
        if has_flag != bool(args.python_enabled):
            raise RuntimeError('Incorrect native Limited API policy: ' + name)
        if args.python_enabled and args.windows and 'Py_NO_LINK_LIB' not in target['definitions'].split(';'):
            raise RuntimeError('Native Python auto-linking is not suppressed: ' + name)
        seen, pending = set(), [name]
        while pending:
            dependency = pending.pop()
            if dependency in seen:
                continue
            seen.add(dependency)
            if dependency == 'Python3::Python':
                raise RuntimeError('Native closure uses versioned Python: ' + name)
            info = targets.get(dependency)
            if not info or info['role'] == 'AGGREGATE':
                continue
            if info['role'] in ('BINDING_SUPPORT', 'MODULE'):
                raise RuntimeError(f'Native target {name} reaches bindings: {dependency}')
            pending.extend(info['links'].split(';'))
    elif role in ('BINDING_SUPPORT', 'MODULE'):
        if 'Py_LIMITED_API' in target['definitions']:
            raise RuntimeError('Bindings compiled with native restrictions: ' + name)
    elif role == 'RUNTIME':
        if target['kind'] != 'INTERFACE_LIBRARY':
            raise RuntimeError('Native runtime requirements became a compiled library')
if not native:
    raise RuntimeError('Policy audit did not find any native targets')
print(f'PASS: {native} native targets; private Limited API, full-API bindings and neutral runtime')
