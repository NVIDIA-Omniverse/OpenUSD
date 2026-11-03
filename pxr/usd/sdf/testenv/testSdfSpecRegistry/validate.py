import ctypes
import os
from pathlib import Path
import sys

build, config, prefix, mode = sys.argv[1:]
build, prefix = Path(build).resolve(), Path(prefix).resolve()
native = Path((build / f'native-{config}.txt').read_text().strip())
handles = []
if sys.platform == 'win32':
    runtime = (build / f'runtime-{config}.txt').read_text().strip()
    handles = [os.add_dll_directory(str(directory))
        for directory in (prefix / 'lib', prefix / 'bin', native.parent, Path(runtime))
        if directory.is_dir()]
if mode == 'raw':
    # The embedding executable created the native specs before initializing
    # Python. Editing a layer in a running interpreter can auto-load _sdf.
    assert 'pxr.Sdf' not in sys.modules
    import sdfSpecRegister, sdfSpecConsume
    sdfSpecRegister.register()
    assert sdfSpecConsume.get() == 'registered in module A'
    assert sdfSpecConsume.get() == 'registered in module A'
    assert 'pxr.Sdf' not in sys.modules
    print('PASS: two Limited API modules use one shared core registry')
else:
    from pxr import Sdf
    library = ctypes.PyDLL(str(native))
    assert library.InitializeSpecs() == 1
    import sdfSpecBindingConsumer as consumer
    prim = consumer.prim()
    attribute = consumer.attribute()
    variant = consumer.variant()
    assert type(prim) is Sdf.PrimSpec
    assert type(attribute) is Sdf.AttributeSpec
    assert type(variant) is Sdf.VariantSpec
    assert type(consumer.variant_prim()) is Sdf.PrimSpec
    assert consumer.echo(None) is None
    assert consumer.echo(attribute) == attribute
    assert consumer.repr(prim) == repr(prim)
    assert consumer.repr(prim).startswith('Sdf.Find(')
    assert eval(repr(prim), {'Sdf': Sdf}) == prim
    consumer.remove()
    assert not prim and prim.expired
    assert consumer.prim() is None
    assert consumer.attribute() is None
    assert consumer.repr(prim) == repr(prim)
    assert 'dormant' in repr(prim)
    # Constructor adapters still create correct spec classes after the split.
    layer = Sdf.Layer.CreateAnonymous()
    parent = Sdf.PrimSpec(layer, 'After', Sdf.SpecifierDef)
    child = Sdf.AttributeSpec(parent, 'a', Sdf.ValueTypeNames.Int)
    assert child.owner == parent
    consumer.clear()
    print('PASS: derived/const/variant/dormant handles, constructors and canonical repr')
