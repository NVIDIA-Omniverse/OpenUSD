import ctypes
import os
from pathlib import Path
import sys
prefix, native, runtime = sys.argv[1:]
handles = []
if sys.platform == 'win32':
    handles = [os.add_dll_directory(str(p)) for p in
        (Path(prefix) / 'lib', Path(prefix) / 'bin', Path(runtime)) if p.is_dir()]
library = ctypes.PyDLL(native)
assert library.CheckRuntimeContracts() == 1
assert all('pxr.' + name not in sys.modules for name in ('Vt', 'Sdf', 'Usd'))
print('PASS: independent backend callbacks, late registration, GIL, pending errors and output contracts')
