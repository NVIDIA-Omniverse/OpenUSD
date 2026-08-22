#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#

import hou


property_name = "ty:convergedSamplesPerPixel"
settings = hou.node("/stage").createNode("rendersettings", "typhoon_settings")
value = settings.parm(hou.text.encodeParm(property_name))
control = settings.parm(hou.text.encodeParm(property_name + "_control"))

assert settings.parmTemplateGroup().findFolder("Typhoon") is not None
assert value is not None
assert control is not None

control.set("set")
value.set(32)
prim = settings.stage().GetPrimAtPath("/Render/rendersettings")
assert prim.HasAPI("TyphoonRenderSettingsAPI")
assert prim.GetAttribute(property_name).Get() == 32
