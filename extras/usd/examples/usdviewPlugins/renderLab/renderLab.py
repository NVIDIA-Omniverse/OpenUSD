"""RenderLab usdview plugin main window."""

import importlib
import sys

from pxr.Usdviewq.qt import QtCore, QtWidgets

from .cameraEditor import CameraEditor
from . import cameraManipulator
from . import domeLightManipulator
from .lightEditor import LightEditor
from .materialEditor import MaterialEditor
from .renderSettingsEditor import RenderSettingsEditor


_window = None
_TAB_BAR_STYLE = """
QTabBar::tab {
    border-top: 2px solid transparent;
}
QTabBar::tab:selected {
    border-top: 2px solid #999999;
}
"""
_REFRESH_BUTTON_WIDTH = 24
_REFRESH_BUTTON_HEIGHT = 24
_SECTION_BY_TAB_INDEX = ("render", "camera", "material")
_RELOAD_MODULES = (
    "renderLab.parameterWidgets",
    "renderLab.renderSettingsMetadata",
    "renderLab.renderSettingsEditor",
    "renderLab.cameraManipulator",
    "renderLab.cameraEditor",
    "renderLab.domeLightManipulator",
    "renderLab.lightEditor",
    "renderLab.materialEditor",
    "renderLab.renderLab",
)


class _CenteredEqualTabBar(QtWidgets.QTabBar):
    """Tab bar with equal tab widths, intended to sit centered in a layout."""

    _tabHorizontalPadding = 24
    _tabVerticalPadding = 8

    def tabSizeHint(self, index):
        size = super().tabSizeHint(index)
        width = size.width()
        metrics = self.fontMetrics()
        for tabIndex in range(self.count()):
            width = max(
                width,
                metrics.horizontalAdvance(self.tabText(tabIndex))
                + self._tabHorizontalPadding)
        size.setWidth(width)
        size.setHeight(size.height() + self._tabVerticalPadding)
        return size


class RenderLabWindow(QtWidgets.QWidget):
    """Top-level RenderLab window opened from usdview."""

    def __init__(self, usdviewApi, parent=None):
        super().__init__(parent)
        self._api = usdviewApi

        self.setWindowTitle("RenderLab")
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose)
        self.setMinimumSize(900, 640)
        self.resize(1060, 900)

        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(6, 6, 6, 6)
        root.setSpacing(6)

        tabRow = QtWidgets.QHBoxLayout()
        tabRow.setContentsMargins(0, 0, 0, 0)
        refreshSpacer = QtWidgets.QWidget()
        refreshSpacer.setFixedWidth(_REFRESH_BUTTON_WIDTH)
        tabRow.addWidget(refreshSpacer)
        tabRow.addStretch(1)
        self._tabBar = _CenteredEqualTabBar()
        self._tabBar.setShape(QtWidgets.QTabBar.RoundedNorth)
        self._tabBar.setExpanding(False)
        self._tabBar.setStyleSheet(_TAB_BAR_STYLE)
        self._tabBar.currentChanged.connect(self._setCurrentPage)
        tabRow.addWidget(self._tabBar)
        tabRow.addStretch(1)
        refreshButton = QtWidgets.QToolButton()
        refreshButton.setIcon(
            self.style().standardIcon(QtWidgets.QStyle.SP_BrowserReload))
        refreshButton.setToolTip("Refresh")
        refreshButton.setFixedSize(_REFRESH_BUTTON_WIDTH, _REFRESH_BUTTON_HEIGHT)
        refreshButton.clicked.connect(self.refresh)
        tabRow.addWidget(refreshButton)
        root.addLayout(tabRow)

        self._stack = QtWidgets.QStackedWidget()
        root.addWidget(self._stack, 1)

        self._renderSettingsEditor = RenderSettingsEditor(usdviewApi, self)
        self._addTab(self._renderSettingsEditor, "Render Settings")

        cameraLight = QtWidgets.QSplitter(QtCore.Qt.Vertical)
        self._cameraEditor = CameraEditor(usdviewApi, cameraLight)
        self._lightEditor = LightEditor(usdviewApi, cameraLight)
        cameraLight.addWidget(self._cameraEditor)
        cameraLight.addWidget(self._lightEditor)
        cameraLight.setSizes([320, 320])
        self._addTab(cameraLight, "Camera && Light")

        self._materialEditor = MaterialEditor(usdviewApi, self)
        self._addTab(self._materialEditor, "Material")

        footer = QtWidgets.QHBoxLayout()
        footer.setContentsMargins(5, 0, 5, 0)
        self._stageLabel = QtWidgets.QLabel()
        self._stageLabel.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self._stageLabel.setStyleSheet("color: #888888; font-size: 11px;")
        self._stageLabel.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        footer.addStretch(1)
        footer.addWidget(self._stageLabel)
        root.addLayout(footer)

        self.refresh()

    def _addTab(self, widget, label):
        self._stack.addWidget(widget)
        self._tabBar.addTab(label)

    def _setCurrentPage(self, index):
        self._stack.setCurrentIndex(index)

    def refresh(self):
        stage = self._api.stage
        rootLayer = stage.GetRootLayer() if stage else None
        rootLayerName = rootLayer.identifier if rootLayer else "<no stage>"
        self._stageLabel.setText("Stage: {}".format(rootLayerName))

        for editor in (
                self._renderSettingsEditor,
                self._cameraEditor,
                self._lightEditor,
                self._materialEditor):
            refresh = getattr(editor, "refresh", None)
            if refresh:
                refresh()

    def setSection(self, section):
        if section == "render":
            self._tabBar.setCurrentIndex(0)
        elif section in ("camera", "light"):
            self._tabBar.setCurrentIndex(1)
            if section == "camera":
                self._cameraEditor.activateEditor()
            else:
                self._lightEditor.activateEditor()
        elif section == "material":
            self._tabBar.setCurrentIndex(2)

    def showEvent(self, event):
        super().showEvent(event)
        self.refresh()


def _clearWindow():
    global _window
    _window = None


def _currentSection():
    if _window is None:
        return None
    try:
        index = _window._tabBar.currentIndex()
    except RuntimeError:
        return None
    if 0 <= index < len(_SECTION_BY_TAB_INDEX):
        return _SECTION_BY_TAB_INDEX[index]
    return None


def _currentLightPath():
    if _window is None:
        return None
    try:
        return _window._lightEditor._currentPath
    except RuntimeError:
        return None
    except AttributeError:
        return None


def _currentCameraPath():
    if _window is None:
        return None
    try:
        return _window._cameraEditor.currentPath()
    except RuntimeError:
        return None
    except AttributeError:
        return None


def _cameraManipulatorEnabled():
    if _window is None:
        return False
    try:
        return _window._cameraEditor.cameraManipulatorEnabled()
    except RuntimeError:
        return False
    except AttributeError:
        return False


def _openRenderLab(usdviewApi, section=None, replaceManipulator=False):
    global _window
    cameraManipulator.SetPreferredCameraPathGetter(_currentCameraPath)
    cameraManipulator.SetEnabledGetter(_cameraManipulatorEnabled)
    cameraManipulator.Install(usdviewApi, replace=replaceManipulator)
    domeLightManipulator.SetPreferredDomeLightPathGetter(_currentLightPath)
    domeLightManipulator.Install(usdviewApi, replace=replaceManipulator)

    if _window is not None:
        try:
            if _window.isVisible():
                if section is not None:
                    _window.setSection(section)
                _window.raise_()
                _window.activateWindow()
                return
        except RuntimeError:
            _window = None

    _window = RenderLabWindow(usdviewApi, parent=usdviewApi.qMainWindow)
    _window.setWindowFlags(QtCore.Qt.Window)
    _window.destroyed.connect(lambda _=None: _clearWindow())
    if section is not None:
        _window.setSection(section)
    _window.show()


def OpenOverview(usdviewApi):
    _openRenderLab(usdviewApi)


def OpenRenderSettings(usdviewApi):
    _openRenderLab(usdviewApi, "render")


def OpenCameraTools(usdviewApi):
    _openRenderLab(usdviewApi, "camera")


def OpenLightTools(usdviewApi):
    _openRenderLab(usdviewApi, "light")


def OpenMaterialTools(usdviewApi):
    _openRenderLab(usdviewApi, "material")


def ReloadRenderLab(usdviewApi):
    section = _currentSection()
    if section is None:
        section = "render"

    window = _window
    if window is not None:
        try:
            window.close()
        except RuntimeError:
            pass
        _clearWindow()

    for moduleName in _RELOAD_MODULES:
        module = sys.modules.get(moduleName)
        if module is not None:
            importlib.reload(module)

    reloadedModule = sys.modules.get(__name__)
    if reloadedModule is None:
        reloadedModule = importlib.import_module(__name__)
    reloadedModule._openRenderLab(
        usdviewApi, section, replaceManipulator=True)
