#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#


def testUsdviewInputFunction(appController):
    assert appController._parserData.cameraLightEnabled is False
    assert appController._dataModel.viewSettings.ambientLightOnly is False
    assert appController._ui.actionAmbient_Only.isChecked() is False

    appController._dataModel.viewSettings.showBBoxes = False
    appController._dataModel.viewSettings.showHUD = False

    appController._ui.actionDomeLight.setChecked(False)
    appController._onDomeLightClicked(False)

    appController._ui.actionDomeLightTexturesVisible.setChecked(False)
    appController._onDomeLightTexturesVisibleClicked(False)

    appController._takeShot("noLights.png")
