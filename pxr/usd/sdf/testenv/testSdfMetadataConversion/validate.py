"""Characterize native metadata conversion independently of Python wrappers."""
import ctypes
import os
from pathlib import Path
import sys

build, config, prefix = sys.argv[1:]
build, prefix = Path(build).resolve(), Path(prefix).resolve()
handles = []
if sys.platform == 'win32':
    runtime = (build / f'runtime-{config}.txt').read_text().strip()
    handles = [os.add_dll_directory(str(directory))
               for directory in (prefix / 'lib', prefix / 'bin', Path(runtime))
               if directory.is_dir()]
native = (build / f'native-{config}.txt').read_text().strip()
library = ctypes.PyDLL(native)
assert 'pxr.Sdf' not in sys.modules and 'pxr.Vt' not in sys.modules
assert library.CheckNativeMetadata() == 1
convert = library.ConvertRawMetadata
convert.argtypes = [ctypes.py_object]
convert.restype = ctypes.py_object
# A missing converter is not negatively cached; loading Vt later enables it.
assert convert([1]) == (False, None, "failed to obtain first element from sequence under key 'value'")
assert 'pxr.Sdf' not in sys.modules and 'pxr.Vt' not in sys.modules
from pxr import Vt, Gf

def succeeds(sequence, expected):
    assert convert(sequence) == (True, expected, ''), (sequence, convert(sequence))

class Sequence:
    def __init__(self, values): self.values = values
    def __len__(self): return len(self.values)
    def __getitem__(self, index): return self.values[index]

class BadItem:
    def __len__(self): return 2
    def __getitem__(self, index): raise ValueError('broken item')

class BadLaterItem:
    def __len__(self): return 2
    def __getitem__(self, index):
        if index == 0: return 1
        raise ValueError('broken later item')

for sequence in ([1, 2], (1, 2), Sequence([1, 2])):
    succeeds(sequence, Vt.IntArray([1, 2]))
for sequence in ([1, 2.5], (1, 2.5), Sequence([1, 2.5])):
    assert convert(sequence) == (False, None,
        "failed to cast sequence element 1: <double> '2.5' under key 'value' to <int>")
succeeds((1.5, 2), Vt.DoubleArray([1.5, 2]))
succeeds((True, 2), Vt.BoolArray([True, True]))
succeeds((1, True), Vt.IntArray([1, 1]))
succeeds((Gf.Vec3f(1), Gf.Vec3f(2)), Vt.Vec3fArray([Gf.Vec3f(1), Gf.Vec3f(2)]))
succeeds((2**63, 2**63 + 1), Vt.UInt64Array([2**63, 2**63 + 1]))
# Explicit Vt wrappers infer a type but do not themselves extract as that scalar.
assert not convert((Vt.UInt64(2**63),))[0]
assert 'cannot infer type from empty sequence' in convert(())[2]
assert convert(object()) == (False, None, "cannot convert python object as sequence under key 'value'")
assert "first sequence element <void>" in convert([None])[2]
for sequence, message in ((BadItem(), 'broken item'), (BadLaterItem(), 'broken later item')):
    try:
        convert(sequence)
    except ValueError as error:
        assert str(error) == message
    else:
        raise AssertionError('Sequence error did not propagate')
    succeeds((1, 2), Vt.IntArray([1, 2]))
assert 'pxr.Sdf' not in sys.modules

from pxr import Sdf
succeeds((Sdf.AssetPath('a'), Sdf.AssetPath('b')),
         Sdf.AssetPathArray([Sdf.AssetPath('a'), Sdf.AssetPath('b')]))
succeeds((Sdf.TimeCode(1), Sdf.TimeCode(2)), Sdf.TimeCodeArray([Sdf.TimeCode(1), Sdf.TimeCode(2)]))
succeeds((Sdf.PathExpression('/a'), Sdf.PathExpression('/b')),
         Sdf.PathExpressionArray([Sdf.PathExpression('/a'), Sdf.PathExpression('/b')]))
# Public wrapper input and raw native input intentionally have different casts.
assert Sdf.ConvertToValidMetadataDictionary({'value': [1, 2.5]}) == (
    True, {'value': Vt.IntArray([1, 2])}, '')
assert Sdf.ConvertToValidMetadataDictionary({'nested': {'bad': ['a', 1]}}) == (
    False, {'nested': {'bad': None}},
    "failed to cast array element 1: <int> '1' under key 'nested:bad' to <string>")
print('PASS: native values, raw typed sequences, diagnostics, errors and late binding')
