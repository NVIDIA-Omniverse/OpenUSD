#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
from pxr import Vt
import array
import sys
import vtArrayHelpers as helpers

Element = helpers.Element
Array = helpers.ElementArray
value = Array([Element(1), Element(2), Element(3)])
assert [e.value for e in value] == [1, 2, 3]
assert [e.value for e in value[::-1]] == [3, 2, 1]
value[1:3] = [Element(7), Element(8)]
assert [e.value for e in value] == [1, 7, 8]
assert [e.value for e in helpers.identity(value)] == [1, 7, 8]
assert [e.value for e in helpers.cast_sequence([Element(4), Element(5)])] == [4, 5]
assert [e.value for e in helpers.cast_sequence(iter([Element(6)]))] == [6]
assert helpers.cast_sequence([object()]) is None
edit = helpers.ElementArrayEditBuilder().Write(Element(9), 0).FinalizeAndReset()
assert [e.value for e in edit.ComposeOver(value)] == [9, 7, 8]
assert [e.value for e in helpers.identity(edit).ComposeOver(value)] == [9, 7, 8]
assert repr(value).startswith("vtArrayHelpers.ElementArray")
assert repr(Array()) == "vtArrayHelpers.ElementArray()"
assert helpers.builtin_name() == "IntArray"
assert helpers.rank(6, 2) == (2, 3)
assert helpers.rank(5, 2)[0] == 1
assert helpers.rank(0, 2) == (2, 0)
assert helpers.rank(6, 0)[0] == 1
if sys.maxsize > 2**32:
    assert helpers.rank(6_000_000_000, 3_000_000_000) == (2, 2)
assert list(helpers.cast_int(memoryview(array.array("d", [1, 2])))) == [1, 2]
assert list(helpers.cast_int([3, 4])) == [3, 4]
assert list(helpers.cast_vector()) == [1.0, 2.5]
buffer = Vt.IntArray([1, 2, 3])
before = sys.getrefcount(buffer)
helpers.request_buffer(buffer, False, False)
for writable, fortran in ((True, False), (False, True)):
    try:
        helpers.request_buffer(buffer, writable, fortran)
    except ValueError:
        pass
    else:
        raise AssertionError("Unsupported buffer request succeeded")
assert sys.getrefcount(buffer) == before
print("PASS: downstream array/edit wrapping, sequence casts, names and shape helper")
