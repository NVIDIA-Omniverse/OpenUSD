import os
from pathlib import Path
import sys

handles = []
if sys.platform == 'win32':
    prefix, runtime = sys.argv[1:]
    handles = [os.add_dll_directory(str(path)) for path in
               (Path(prefix) / 'lib', Path(prefix) / 'bin', Path(runtime))
               if path.is_dir()]
from pxr import Tf
assert 'pxr.Pcp' not in sys.modules and 'pxr.Usd' not in sys.modules
import pcpBindingHelpers as helper

original = {'original': ['unchanged']}
assert helper.convert({}) == (True, original, [])
for sequence in (['one', 'two'], ('one', 'two')):
    assert helper.convert({'choice': sequence}) == (
        True, dict(original, choice=['one', 'two']), [])
assert helper.convert({'original': ['replacement']}) == (
    True, {'original': ['replacement']}, [])
assert helper.convert({'': ['ignored'], 'choice': [], 'original': []}) == (
    True, original, [])
assert helper.convert({'caf\u00e9': ['v\u00e9']}) == (
    True, dict(original, **{'caf\u00e9': ['v\u00e9']}), [])
for value, kind in (({1: ['x']}, 'key'), ({'a': 1}, 'val'),
                    ({'a': None}, 'val'), ({'a': 'xy'}, 'val'),
                    ({'a': b'xy'}, 'val')):
    assert helper.convert(value) == (
        False, original, ['unrecognized type for PcpVariantFallbackMap ' + kind])
for invalid, kind in (({1: ['x']}, 'key'), ({'bad': 1}, 'val')):
    # This helper historically retains earlier updates on failure.
    assert helper.convert({'valid': ['value'], **invalid}) == (
        False, dict(original, valid=['value']),
        ['unrecognized type for PcpVariantFallbackMap ' + kind])
for sequence in ([1], ['x', 1]):
    try:
        helper.convert({'choice': sequence})
    except TypeError:
        pass
    else:
        raise AssertionError('Invalid sequence element must retain TypeError')
assert helper.convert({'afterError': ['ok']}) == (
    True, dict(original, afterError=['ok']), [])
assert 'pxr.Pcp' not in sys.modules and 'pxr.Usd' not in sys.modules
print('PASS: standalone conversion, diagnostics, partial updates and exceptions')

from pxr import Pcp, Sdf, Usd
layer = Sdf.Layer.CreateAnonymous()
cache = Pcp.Cache(Pcp.LayerStackIdentifier(layer))
saved = Usd.Stage.GetGlobalVariantFallbacks()
try:
    for setter, getter in (
        (cache.SetVariantFallbacks, cache.GetVariantFallbacks),
        (Usd.Stage.SetGlobalVariantFallbacks, Usd.Stage.GetGlobalVariantFallbacks)):
        setter({'choice': ('one', 'two')})
        expected = {'choice': ['one', 'two']}
        assert getter() == expected
        try:
            setter({'valid': ['value'], 'invalid': 1})
        except Tf.ErrorException as error:
            assert 'unrecognized type for PcpVariantFallbackMap val' in str(error)
        else:
            raise AssertionError('Invalid fallback map must report a coding error')
        assert getter() == expected
        setter({})
        assert getter() == {}
finally:
    Usd.Stage.SetGlobalVariantFallbacks(saved)
print('PASS: Pcp cache and Usd stage consumers, including failure rollback')
