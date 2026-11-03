import sys
import os
from pathlib import Path
handles = []
if sys.platform == 'win32':
    prefix, runtime = sys.argv[1:]
    handles = [os.add_dll_directory(str(path)) for path in
               (Path(prefix) / 'lib', Path(prefix) / 'bin', Path(runtime)) if path.is_dir()]
assert 'pxr.Sdf' not in sys.modules
import sdfBindingHelpers as helper
assert helper.convert({'format': 'usda'}, True) == (True, {'format': 'usda'}, 'preset')
assert helper.convert({}, True) == (True, {}, 'preset')
assert helper.convert({'clé': 'value'}, True) == (True, {'clé': 'value'}, 'preset')
original = {'original': 'unchanged'}
for value, message in (({1: 'x'}, 'All file format argument keys must be strings'),
                       ({'format': 1}, 'All file format argument values must be strings'),
                       ({'valid': 'value', 'invalid': None}, 'All file format argument values must be strings')):
    assert helper.convert(value, True) == (False, original, message)
    assert helper.convert(value, False) == (False, original, 'preset')
assert 'pxr.Sdf' not in sys.modules
print('PASS: standalone inline argument conversion, diagnostics and transactional output')
