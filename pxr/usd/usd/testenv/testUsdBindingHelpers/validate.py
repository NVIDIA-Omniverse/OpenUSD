import os
from pathlib import Path
import sys
prefix, runtime = sys.argv[1:]
handles = []
if sys.platform == 'win32':
    handles = [os.add_dll_directory(str(p)) for p in
        (Path(prefix) / 'lib', Path(prefix) / 'bin', Path(runtime)) if p.is_dir()]
from pxr import Usd, Sdf, Vt, UsdShade
import usdBindingHelpers as helper

assert helper.convert(None, 'int') is None
assert helper.convert([1, 2.5], 'int[]') == Vt.IntArray([1, 2])
assert helper.metadata({'value': [1, 2.5]}, 'customData') == {'value': Vt.IntArray([1, 2])}
assert helper.metadata(['a', 'b'], 'primChildren') == ['a', 'b']
try:
    helper.metadata(1, 'documentation')
except ValueError as error:
    assert "Expected type 'string'" in str(error)
else:
    raise AssertionError('Metadata adapter must retain the conversion error')

stage = Usd.Stage.CreateInMemory()
root = stage.GetRootLayer()
layer = Sdf.Layer.CreateAnonymous()
root.subLayerPaths.append(layer.identifier)
context = helper.context(stage, layer)
assert isinstance(context, Usd.EditContext)
assert stage.GetEditTarget().GetLayer() == root
with context as entered:
    assert entered is context
    assert stage.GetEditTarget().GetLayer() == layer
    stage.DefinePrim('/InLayer')
assert layer.GetPrimAtPath('/InLayer')
assert stage.GetEditTarget().GetLayer() == root
try:
    with helper.context(stage, layer):
        raise LookupError('leave context')
except LookupError:
    pass
assert stage.GetEditTarget().GetLayer() == root

# The default target is captured on entry, not while constructing the carrier.
context = helper.default_context(stage)
stage.SetEditTarget(Usd.EditTarget(layer))
with context:
    assert stage.GetEditTarget().GetLayer() == layer
    stage.SetEditTarget(Usd.EditTarget(root))
assert stage.GetEditTarget().GetLayer() == layer
stage.SetEditTarget(Usd.EditTarget(root))

prim = stage.DefinePrim('/Variant')
variant = prim.GetVariantSets().AddVariantSet('choice')
variant.AddVariant('one')
variant.SetVariantSelection('one')
with helper.variant_context(variant, root):
    stage.DefinePrim('/Variant/Child')
path = Sdf.Path('/Variant').AppendVariantSelection('choice', 'one').AppendChild('Child')
assert root.GetPrimAtPath(path)
assert stage.GetEditTarget().GetLayer() == root

# UsdShade must return the same carrier type registered by _usd as well.
material = UsdShade.Material.Define(stage, '/Material')
context = material.GetEditContextForVariant('one')
assert isinstance(context, Usd.EditContext)
with context:
    material.GetPrim().CreateAttribute('inVariant', Sdf.ValueTypeNames.Int).Set(42)
assert stage.GetEditTarget().GetLayer() == root
print('PASS: external conversion adapters and shared edit contexts, variants, exceptions and UsdShade')
