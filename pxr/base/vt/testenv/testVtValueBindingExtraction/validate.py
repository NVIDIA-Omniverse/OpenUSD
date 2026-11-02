#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
import gc
import sys
import weakref
from pxr import Vt
import vtValueConsume as consume

# The native library has already constructed values, without any knowledge of
# Boost or of the Python registration module. Failed lookups must not be cached.
for convert in (consume.stored, consume.typed, consume.erased, consume.native_view):
    try:
        convert()
    except TypeError:
        pass
    else:
        raise AssertionError("Unknown native type converted before registration")

import vtValueRegister as registration
# Only Boost class registration: no VtValueFromPython registration is required
# for runtime lookup of these native C++ values.
for convert in (consume.stored, consume.typed, consume.erased, consume.native_view):
    assert convert().number == 37
assert consume.pointer(False).number == 37
assert consume.pointer(True) is None
assert consume.null_shared() is None
assert consume.owned_shared().number == 91
wrapped = registration.NativeValue(42)
assert consume.shared(wrapped) is wrapped
registration.register_from_python()
assert consume.ref(wrapped).number == 42
assert consume.ref(17) == 17
assert consume.ref("abc") == "abc"
assert consume.ref([1, 2, 3]) == [1, 2, 3]
assert consume.ref(Vt.IntArray([1, 2])) == Vt.IntArray([1, 2])

class PythonObject:
    pass

obj = PythonObject()
reference = weakref.ref(obj)
before = sys.getrefcount(obj)
for _ in range(20):
    assert consume.python_object(obj) is obj
    assert consume.ref(obj) is obj
assert sys.getrefcount(obj) == before
del obj
gc.collect()
assert reference() is None

expected = [True, "x", -7, 255, -12, 65535, -123, 123, -123, 123,
            -9223372036854775807, 18446744073709551615,
            1.25, 2.5, 3.75, "a\\0b", "wide", 1+2j, 3+4j, 5+6j]
expected[15] = "a" + chr(0) + "b"
for index, value in enumerate(expected):
    result = consume.builtin(index)
    assert result == value, (index, result, value)
    assert type(result) is type(value), (index, type(result), type(value))
assert consume.builtin(100) is None
print("PASS: late class registration, values/proxies/views, builtin parity and Python ownership")
