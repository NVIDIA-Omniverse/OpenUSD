#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#

import unittest

from pxr import Plug, UsdImagingGL
from pxr.Usdviewq.qt import QtWidgets

from renderLab import renderSettingsMetadata
from renderLab.renderSettingsEditor import RenderSettingsEditor


class _StageView:
    def __init__(self, aovs=None):
        self.aovs = list(aovs or ["color", "depth", "normal"])
        self.rendererAovName = "color"
        self.failedAovs = set()
        self.setAovCalls = []
        self.settings = []
        self.settingValues = {}

    def GetRendererAovs(self):
        return list(self.aovs)

    def SetRendererAov(self, aov):
        aov = str(aov)
        self.setAovCalls.append(aov)
        if aov in self.failedAovs:
            return False
        self.rendererAovName = aov
        return True

    def GetRendererSettingsList(self):
        return list(self.settings)

    def GetRendererSetting(self, key):
        return self.settingValues.get(str(key))

    def SetRendererSetting(self, key, value):
        self.settingValues[str(key)] = value


class _RendererSetting:
    def __init__(self, key, name, settingType, defaultValue):
        self.key = key
        self.name = name
        self.type = settingType
        self.defValue = defaultValue


class _UsdviewApi:
    def __init__(self, rendererId="HdEmbreeRendererPlugin"):
        self.rendererId = rendererId
        self.stageView = _StageView()

    def GetViewportCurrentRendererId(self):
        return self.rendererId


class TestRenderLabRenderSettings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._application = (
            QtWidgets.QApplication.instance() or QtWidgets.QApplication([]))

    def setUp(self):
        self.api = _UsdviewApi()
        self.editor = RenderSettingsEditor(self.api)

    def tearDown(self):
        self.editor.close()
        self.editor.deleteLater()
        self._application.processEvents()

    def test_PluginIsInstalledAndLoadable(self):
        plugin = Plug.Registry().GetPluginWithName("renderLab")
        self.assertIsNotNone(plugin)
        plugin.Load()

    def _items(self):
        combo = self.editor._aovCombo
        return [
            (combo.itemText(index), str(combo.itemData(index)))
            for index in range(combo.count())
        ]

    def _select(self, token):
        combo = self.editor._aovCombo
        for index in range(combo.count()):
            if str(combo.itemData(index)) == token:
                combo.setCurrentIndex(index)
                self._application.processEvents()
                return
        self.fail("Missing AOV token: {}".format(token))

    def _groupNames(self):
        names = []
        for row in range(self.editor._form.rowCount()):
            item = self.editor._form.itemAt(
                row, QtWidgets.QFormLayout.SpanningRole)
            widget = item.widget() if item else None
            if isinstance(widget, QtWidgets.QToolButton):
                names.append(widget.text())
        return names

    def _groupLabels(self, groupName):
        form = self.editor._form
        for row in range(form.rowCount() - 1):
            item = form.itemAt(row, QtWidgets.QFormLayout.SpanningRole)
            button = item.widget() if item else None
            if not isinstance(button, QtWidgets.QToolButton):
                continue
            if button.text() != groupName:
                continue
            contentItem = form.itemAt(
                row + 1, QtWidgets.QFormLayout.SpanningRole)
            content = contentItem.widget()
            layout = content.layout()
            return [
                layout.itemAt(index, QtWidgets.QFormLayout.LabelRole)
                    .widget().text()
                for index in range(layout.rowCount())
            ]
        self.fail("Missing group: {}".format(groupName))

    def test_AovMetadataIsOrderedAndCopied(self):
        metadata = renderSettingsMetadata.getAovMetadata(
            "HdEmbreeRendererPlugin")
        self.assertEqual(
            [(aov["label"], aov["token"]) for aov in metadata],
            [
                ("adaptiveHeatmap", "adaptiveHeatmap"),
                ("ambocc", "ambocc"),
            ])
        metadata[0]["label"] = "Changed"
        self.assertEqual(
            renderSettingsMetadata.getAovMetadata(
                "HdEmbreeRendererPlugin")[0]["label"],
            "adaptiveHeatmap")
        self.assertEqual(renderSettingsMetadata.getAovMetadata("unknown"), [])

    def test_CombinesStandardAndRendererAovs(self):
        self.assertEqual(
            self._items(),
            [
                ("color", "color"),
                ("depth", "depth"),
                ("normal", "normal"),
                ("adaptiveHeatmap", "adaptiveHeatmap"),
                ("ambocc", "ambocc"),
            ])
        self.assertEqual(self.editor._aovCombo.currentText(), "color")
        self.assertEqual(self.api.stageView.setAovCalls, [])

    def test_AovGroupReplacesDiagnosticsAndTileSizeEndsOther(self):
        settingType = UsdImagingGL.RendererSettingType
        self.api.stageView.settings = [
            _RendererSetting(
                "ty:convergedSamplesPerPixel",
                "Samples To Convergence",
                settingType.INT,
                256),
            _RendererSetting(
                "ty:materialRenderContext",
                "Material Render Context",
                settingType.STRING,
                "mtlx"),
            _RendererSetting(
                "renderingColorSpace",
                "Rendering Color Space",
                settingType.STRING,
                "lin_rec709_scene"),
            _RendererSetting(
                "ty:minCurveWidth",
                "Minimum Curve Width",
                settingType.FLOAT,
                0.001),
            _RendererSetting(
                "unmappedSetting",
                "Unmapped Setting",
                settingType.INT,
                1),
            _RendererSetting(
                "ty:tileSize",
                "Tile Size",
                settingType.INT,
                8),
        ]
        self.editor.refresh()

        self.assertEqual(
            self._groupNames(),
            ["Sampling", "Materials", "AOV", "Scene", "Other"])
        self.assertNotIn("Diagnostics", self._groupNames())
        self.assertEqual(
            self._groupLabels("AOV"), ["Viewport AOV"])
        self.assertEqual(
            self._groupLabels("Scene"),
            ["Rendering Color Space", "Minimum Curve Width"])
        self.assertEqual(
            renderSettingsMetadata.getSettingMetadata(
                "HdEmbreeRendererPlugin", "ty:minCurveWidth"),
            {"category": "Scene", "order": 50})
        otherSettingRow = next(iter(self.editor._rowsByWidget.values()))
        self.assertEqual(
            self.editor._aovLabel.styleSheet(),
            otherSettingRow.labelWidget.styleSheet())
        self.assertEqual(
            self._groupLabels("Other"),
            ["Unmapped Setting", "Tile Size"])

    def test_MetadataLabelOverridesDuplicateStandardToken(self):
        self.api.stageView.aovs.append("adaptiveHeatmap")
        self.editor.refresh()
        items = self._items()
        self.assertEqual(
            [item for item in items if item[1] == "adaptiveHeatmap"],
            [("adaptiveHeatmap", "adaptiveHeatmap")])

    def test_SelectsViewportAovThroughStageView(self):
        self._select("adaptiveHeatmap")
        self.assertEqual(
            self.api.stageView.setAovCalls, ["adaptiveHeatmap"])
        self.assertEqual(
            self.api.stageView.rendererAovName, "adaptiveHeatmap")
        self.assertEqual(
            str(self.editor._aovCombo.currentData()), "adaptiveHeatmap")

    def test_FailedSelectionRestoresCurrentAov(self):
        self.api.stageView.failedAovs.add("ambocc")
        self._select("ambocc")
        self.assertEqual(self.api.stageView.setAovCalls, ["ambocc"])
        self.assertEqual(self.api.stageView.rendererAovName, "color")
        self.assertEqual(str(self.editor._aovCombo.currentData()), "color")

    def test_ExternalCustomAovIsPreservedAcrossRefresh(self):
        self.api.stageView.rendererAovName = "rendererSpecific"
        self.editor._refreshIfRendererChanged()
        self.assertIn(
            ("<Custom: rendererSpecific>", "rendererSpecific"),
            self._items())
        self.assertEqual(
            str(self.editor._aovCombo.currentData()), "rendererSpecific")

        self.editor.refresh()
        self.assertEqual(
            str(self.editor._aovCombo.currentData()), "rendererSpecific")
        self.assertEqual(self.api.stageView.setAovCalls, [])

        self.api.stageView.rendererAovName = "color"
        self.editor._refreshIfRendererChanged()
        self.assertNotIn(
            ("<Custom: rendererSpecific>", "rendererSpecific"),
            self._items())
        self.assertEqual(str(self.editor._aovCombo.currentData()), "color")

    def test_RendererChangeRemovesEmbreeAovs(self):
        self.api.rendererId = "HdStormRendererPlugin"
        self.api.stageView.rendererAovName = "color"
        self.editor._refreshIfRendererChanged()
        self.assertEqual(
            self._items(),
            [
                ("color", "color"),
                ("depth", "depth"),
                ("normal", "normal"),
            ])
        self.assertEqual(self.editor._rendererLabel.text(), "Storm")
        self.assertEqual(self.api.stageView.setAovCalls, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
