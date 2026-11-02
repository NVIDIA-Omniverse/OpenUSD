import ctypes
import os
from pathlib import Path
import sys

build, config, prefix, dependency_dir = sys.argv[1:]
build = Path(build)
native = Path((build / f"native-{config}.txt").read_text().strip())
handles = []
if sys.platform == "win32":
    for directory in (native.parent, Path(prefix) / "lib", dependency_dir):
        handles.append(os.add_dll_directory(str(directory)))
assert "pxr.Ar" not in sys.modules
library = ctypes.CDLL(str(native))
assert library.EarlyContextValue() == 17

from pxr import Ar
import arContextConsume

early = arContextConsume.GetEarly()
for operation in (early.Get, lambda: repr(early)):
    try:
        operation()
    except TypeError:
        pass
    else:
        raise AssertionError("Unwrapped context must report a missing converter")

from arContextRegister import LateContext
assert early.Get()[0].value == 17
assert "LateContext(17)" in repr(early)
assert arContextConsume.GetEarly() == early
# Conversion returns a copy, not a reference into the native context.
early.Get()[0].value = 99
assert early.Get()[0].value == 17
for value in (LateContext(23), [LateContext(23)], (LateContext(23),)):
    ctx = Ar.ResolverContext(value)
    assert ctx.Get()[0].value == 23
    assert Ar._TestImplicitConversion(value) == ctx
assert Ar.ResolverContext(None).IsEmpty()
assert Ar.ResolverContext([]).IsEmpty()
print("PASS: native early creation, late binding, error recovery and round trips")
