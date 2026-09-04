#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#

import hou
from pxr import Sdf


settings = hou.node("/stage").createNode("rendersettings", "typhoon_settings")
samples_name = "ty:convergedSamplesPerPixel"
samples = settings.parm(hou.text.encodeParm(samples_name))
samples_control = settings.parm(hou.text.encodeParm(samples_name + "_control"))
minimum_width_name = "ty:minCurveWidth"
minimum_width_parm_name = hou.text.encodeParm(minimum_width_name)
minimum_width = settings.parm(minimum_width_parm_name)
minimum_width_control = settings.parm(
    hou.text.encodeParm(minimum_width_name + "_control"))
minimum_width_template = settings.parmTemplateGroup().find(
    minimum_width_parm_name)

assert settings.parmTemplateGroup().findFolder("Typhoon") is not None
assert samples is not None
assert samples_control is not None
assert minimum_width is not None
assert minimum_width_control is not None
assert minimum_width_template is not None
assert minimum_width_template.type() == hou.parmTemplateType.Float
assert minimum_width_template.numComponents() == 1
assert minimum_width_template.defaultValue() == (0.001,)
assert abs(minimum_width.eval() - 0.001) < 1.0e-9

samples_control.set("set")
samples.set(32)
minimum_width_control.set("set")
minimum_width.set(0.02)
prim = settings.stage().GetPrimAtPath("/Render/rendersettings")
assert prim.HasAPI("TyphoonRenderSettingsAPI")
assert prim.GetAttribute(samples_name).Get() == 32
minimum_width_attribute = prim.GetAttribute(minimum_width_name)
assert minimum_width_attribute.GetTypeName() == Sdf.ValueTypeNames.Float
assert abs(minimum_width_attribute.Get() - 0.02) < 1.0e-6
