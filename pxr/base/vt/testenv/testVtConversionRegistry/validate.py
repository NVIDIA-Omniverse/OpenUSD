#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Run the two-module conversion contract against an installed SDK."""
import gc
import sys
from pxr import Vt
import vtRegistryConsume as consume
import vtRegistryRegister as registration

registered = registration.Registered(7)
convertible = registration.Convertible(5)
assert consume.probe(registered) == -1
assert consume.read(registered) == -1
registration.register_lvalue()
assert consume.probe(registered) == 7
assert consume.read(registered) == 7
assert consume.probe(convertible) == -1
assert consume.read(convertible) == -1
registration.register_all()
for operation in (consume.probe, consume.read):
    assert operation(registered) == 7
    assert operation(convertible) == 105
    before = sys.getrefcount(registered)
    for _ in range(20):
        assert operation(registered) == 7
    assert sys.getrefcount(registered) == before
    try:
        operation(registration.Convertible(-1))
    except ValueError as error:
        assert "negative conversion" in str(error)
    else:
        raise AssertionError("Conversion error was swallowed")
    assert operation(convertible) == 105
roundtrip = consume.echo(registered)
assert roundtrip.value == 7
del registered
gc.collect()
assert roundtrip.value == 7
assert consume.echo(convertible).value == 105
print("PASS: shared registry across modules; lvalue/rvalue, errors and roundtrip ownership")
