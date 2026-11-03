"""Exercise public conversion semantics without importing the Usd bindings."""
import argparse
import array
import ctypes
import json
import os
from pathlib import Path
import sys

parser = argparse.ArgumentParser()
parser.add_argument('prefix')
parser.add_argument('native')
parser.add_argument('runtime')
parser.add_argument('--record')
args = parser.parse_args()
prefix = Path(args.prefix)
handles = []
if sys.platform == 'win32':
    handles = [os.add_dll_directory(str(p)) for p in
        (prefix / 'lib', prefix / 'bin', Path(args.runtime)) if p.is_dir()]
library = ctypes.PyDLL(args.native)
convert = library.ConvertType
convert.argtypes = [ctypes.py_object, ctypes.c_char_p]
convert.restype = ctypes.py_object
metadata = library.ConvertMetadata
metadata.argtypes = [ctypes.py_object, ctypes.c_char_p, ctypes.c_char_p]
metadata.restype = ctypes.py_object
assert 'pxr.Vt' not in sys.modules and 'pxr.Usd' not in sys.modules
try:
    convert(42, b'int')
except TypeError:
    pass
else:
    raise AssertionError('Missing backend must report a conversion error')
assert 'pxr.Vt' not in sys.modules and 'pxr.Usd' not in sys.modules
from pxr import Vt, Gf, Sdf

def type_value(value, target):
    return convert(value, target.encode())[1]
def meta(value, key='customData', path=''):
    return metadata(value, key.encode(), path.encode())

assert type_value(None, 'int') is None
assert type_value(2.5, 'int') == 2
assert type_value([1, 2.5], 'int[]') == Vt.IntArray([1, 2])
assert type_value('text', 'token') == 'text'
assert type_value(2**63, 'uint64') == 2**63
assert type_value(Gf.Vec3f(1), 'double3') == Gf.Vec3d(1)
assert type_value(memoryview(array.array('i', [3, 4])), 'int[]') == Vt.IntArray([3, 4])
opaque = object()
assert type_value(opaque, 'int') is opaque
assert meta(None, 'documentation') == (True, ('void', None), [])
assert meta(1, 'hidden')[1][1] is True
assert meta(['a', 'b'], 'primChildren')[1][1] == ['a', 'b']
assert meta(Vt.StringArray(['a', 'b']), 'subLayers')[1][1] == ['a', 'b']
# A checked miss keeps the inferred value; this is existing metadata behavior.
assert meta(1, 'subLayers')[1] == ('int', 1)
assert meta({'x': [1, 2.5]})[1][1] == {'x': Vt.IntArray([1, 2])}
assert meta(42, path='outer:inner')[1] == ('int', 42)
assert meta(None, 'missing') == (False, ('string', 'unchanged'),
                                ['Unregistered metadata key: missing'])
assert library.OutputUnchanged()

class Opaque:
    def __repr__(self): return 'Opaque()'
class Sequence:
    def __len__(self): return 2
    def __getitem__(self, i):
        if i >= 2: raise IndexError
        return [1, 2.5][i]
    def __repr__(self): return 'Sequence()'
class BrokenSequence:
    def __len__(self): return 2
    def __getitem__(self, i): raise ValueError('broken item')
    def __repr__(self): return 'BrokenSequence()'
class BrokenRepr:
    def __repr__(self): raise RuntimeError('broken repr')

records = []
expected_outcomes = json.loads(Path(__file__).with_name('outcomes.json').read_text())['outcomes']
def record(name, function):
    try:
        result = function()
        records.append((name, 'ok', repr(result)))
    except Exception as error:
        records.append((name, type(error).__name__, str(error)))
        if not name.startswith('type:'):
            assert library.OutputUnchanged(), name
    assert records[-1][1] == expected_outcomes[name], records[-1]

values = [None, True, 1, 2.5, 'text', ['a', 'b'], Vt.StringArray(['a', 'b']),
          {'x': [1, 2.5]}, Opaque(), Sequence(), BrokenSequence()]
for key in ('documentation', 'customData', 'subLayers', 'primChildren', 'hidden',
            'allowedTokens', 'apiSchemas'):
    for i, value in enumerate(values):
        record(f'{key}:{i}', lambda value=value, key=key: meta(value, key))
for i, value in enumerate((1, [1, 2], Opaque(), {'x': True})):
    record('keypath:' + str(i), lambda value=value: meta(value, path='outer:inner'))
for i, value in enumerate((1, Vt.Int(1), Vt.Float(2.5), Sdf.AssetPath('asset'),
                           Gf.Vec3f(1), [1, 2.5], Opaque())):
    record('type:' + str(i), lambda value=value: convert(value, b'int[]'))
# Error classes and field/key-path diagnostics are part of the public contract.
for value in (1, Vt.StringArray(['x'])):
    try: meta(value, 'primChildren')
    except TypeError as error:
        assert 'No registered converter' in str(error)
        assert library.OutputUnchanged()
    else: raise AssertionError('Token vectors require backend extraction')
try: meta([1, 2], path='outer:inner')
except ValueError as error:
    assert str(error) == "Invalid value type for dictionary key-path 'customData:outer:inner': '[1, 2]'."
    assert library.OutputUnchanged()
else: raise AssertionError('Dictionary key-path must validate the map value')
assert meta('after error', 'documentation')[1][1] == 'after error'
try: meta(BrokenRepr(), 'documentation')
except ValueError as error:
    assert '<invalid repr>' in str(error)
    assert library.OutputUnchanged()
else: raise AssertionError('A broken repr must retain the metadata ValueError')
assert 'pxr.Usd' not in sys.modules
if args.record:
    Path(args.record).write_text(json.dumps(records, indent=2))
print('PASS: conversion/casting/buffers, metadata fallbacks, errors and output ownership;', len(records), 'characterized cases')
