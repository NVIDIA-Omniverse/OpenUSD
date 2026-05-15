"""Hydra renderer setting controls for RenderLab."""

from pxr import UsdImagingGL
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from .parameterWidgets import (
    BoolControl,
    FloatControl,
    IntControl,
    NoWheelComboBox,
    ParameterRow,
    valuesEqual,
)
from . import renderSettingsMetadata


_INT_RANGE = (-2 ** 31, 2 ** 31 - 1)
_FLOAT_RANGE = (-2 ** 31, 2 ** 31 - 1)
_CONTROL_WIDTH = 150
_SPINBOX_WIDTH = 104
_FLOAT_SPINBOX_WIDTH = _SPINBOX_WIDTH
_INT_SPINBOX_WIDTH = _SPINBOX_WIDTH
_STRING_WIDTH = 260
_COMBO_BOX_WIDTH = _CONTROL_WIDTH
_EDITOR_PANE_WIDTH = 730
_GROUP_HEADER_STYLE = (
    "QToolButton {"
    " font-weight: bold;"
    " border: none;"
    " border-radius: 3px;"
    " color: #999999;"
    " background-color: #2d2d2d;"
    " padding: 3px 6px;"
    " margin-top: 0px;"
    " text-align: left;"
    "}"
    "QToolButton:hover { background-color: #333; border-color: #777; }")
_GROUP_CONTENT_LEFT_MARGIN = 12
_LABEL_WIDTH_REFERENCE = "transmission_dispersion_abbe_number"
_LABEL_WIDTH_PADDING = 16
_RENDERER_LABEL_STYLE = "font-weight: bold; color: #999999;"


class RenderSettingsEditor(QtWidgets.QWidget):
    """Lists and edits settings exposed by the current Hydra renderer."""

    def __init__(self, usdviewApi, parent=None):
        super().__init__(parent)
        self._api = usdviewApi
        self._updating = False
        self._originalValues = {}
        self._rowsByWidget = {}
        self._lastRendererId = None
        self._settingLabelWidth = 0
        self._controlHeight = None

        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(0)

        pane = QtWidgets.QWidget()
        pane.setFixedWidth(_EDITOR_PANE_WIDTH)
        paneLayout = QtWidgets.QVBoxLayout(pane)
        paneLayout.setContentsMargins(0, 0, 4, 0)
        paneLayout.setSpacing(0)

        toolbar = QtWidgets.QHBoxLayout()
        toolbar.setContentsMargins(_GROUP_CONTENT_LEFT_MARGIN, 0, 0, 0)
        self._rendererLabel = QtWidgets.QLabel()
        self._rendererLabel.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse)
        self._rendererLabel.setStyleSheet(_RENDERER_LABEL_STYLE)
        toolbar.addWidget(self._rendererLabel, 1)
        paneLayout.addLayout(toolbar)

        headerSpacerRow = QtWidgets.QHBoxLayout()
        headerSpacerRow.setContentsMargins(_GROUP_CONTENT_LEFT_MARGIN, 0, 0, 0)
        headerSpacerLabel = QtWidgets.QLabel(" ")
        headerSpacerLabel.setWordWrap(True)
        headerSpacerRow.addWidget(headerSpacerLabel, 1)
        paneLayout.addLayout(headerSpacerRow)
        paneLayout.addSpacing(5)

        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        paneLayout.addWidget(self._scroll, 1)
        root.addWidget(pane, 1, QtCore.Qt.AlignLeft)

        self._formContainer = QtWidgets.QWidget()
        self._form = QtWidgets.QFormLayout(self._formContainer)
        self._form.setFieldGrowthPolicy(
            QtWidgets.QFormLayout.ExpandingFieldsGrow)
        self._form.setLabelAlignment(QtCore.Qt.AlignLeft)
        self._form.setContentsMargins(11, 0, 11, 11)
        self._scroll.setWidget(self._formContainer)

        self._pollTimer = QtCore.QTimer(self)
        self._pollTimer.timeout.connect(self._refreshIfRendererChanged)
        self._pollTimer.start(1000)

        self.refresh()

    def refresh(self):
        stageView = self._stageView()
        rendererId = self._rendererId()
        self._lastRendererId = rendererId
        self._rendererLabel.setText(
            renderSettingsMetadata.getRendererDisplayName(rendererId))

        self._clearForm()

        if not stageView:
            self._form.addRow(QtWidgets.QLabel("No stage view"))
            return

        settings = renderSettingsMetadata.orderSettings(
            rendererId,
            list(stageView.GetRendererSettingsList()))
        if not settings:
            self._form.addRow(QtWidgets.QLabel("No renderer settings"))
            return

        self._settingLabelWidth = self._computeSettingLabelWidth(
            [setting.name for setting in settings])

        self._updating = True
        useCategories = renderSettingsMetadata.hasRendererMetadata(rendererId)
        currentCategory = None
        currentLayout = self._form
        for setting in settings:
            settingMetadata = renderSettingsMetadata.getSettingMetadata(
                rendererId, setting.key)
            if useCategories:
                category = settingMetadata.get("category", "Other")
                if category != currentCategory:
                    currentLayout = self._addCollapsibleGroup(category)
                    currentCategory = category
            self._addSetting(setting, settingMetadata, currentLayout)
        self._updating = False

    def closeEvent(self, event):
        self._pollTimer.stop()
        super().closeEvent(event)

    def _refreshIfRendererChanged(self):
        if self._rendererId() != self._lastRendererId:
            self.refresh()

    def _stageView(self):
        stageView = getattr(self._api, "stageView", None)
        if stageView:
            return stageView

        stageView = getattr(self._api, "_stageView", None)
        if stageView:
            return stageView

        appController = getattr(self._api, "_UsdviewApi__appController", None)
        if appController:
            return getattr(appController, "_stageView", None)

        return None

    def _rendererId(self):
        getRendererId = getattr(self._api, "GetViewportCurrentRendererId", None)
        if getRendererId:
            return str(getRendererId())

        stageView = self._stageView()
        if stageView:
            return str(stageView.GetCurrentRendererId())

        return ""

    def _clearForm(self):
        self._rowsByWidget = {}
        while self._form.rowCount():
            self._form.removeRow(0)

    def _addCollapsibleGroup(self, category):
        button = QtWidgets.QToolButton()
        button.setText(category)
        button.setCheckable(True)
        button.setChecked(True)
        button.setArrowType(QtCore.Qt.DownArrow)
        button.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
        button.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding,
            QtWidgets.QSizePolicy.Fixed)
        button.setStyleSheet(_GROUP_HEADER_STYLE)

        content = QtWidgets.QWidget()
        layout = QtWidgets.QFormLayout(content)
        layout.setFieldGrowthPolicy(
            QtWidgets.QFormLayout.ExpandingFieldsGrow)
        layout.setLabelAlignment(QtCore.Qt.AlignLeft)
        layout.setContentsMargins(_GROUP_CONTENT_LEFT_MARGIN, 2, 0, 6)
        layout.setHorizontalSpacing(12)
        layout.setVerticalSpacing(5)

        def toggled(checked):
            content.setVisible(checked)
            button.setArrowType(
                QtCore.Qt.DownArrow if checked else QtCore.Qt.RightArrow)
            self._formContainer.adjustSize()

        button.toggled.connect(toggled)
        self._form.addRow(button)
        self._form.addRow(content)
        return layout

    def _computeSettingLabelWidth(self, names):
        referenceNames = list(names) + [_LABEL_WIDTH_REFERENCE]
        normalMetrics = QtGui.QFontMetrics(self.font())
        italicFont = QtGui.QFont(self.font())
        italicFont.setItalic(True)
        italicMetrics = QtGui.QFontMetrics(italicFont)
        return max(
            max(normalMetrics.horizontalAdvance(name) for name in referenceNames),
            max(italicMetrics.horizontalAdvance(name) for name in referenceNames)
        ) + _LABEL_WIDTH_PADDING

    def _addSetting(self, setting, settingMetadata, layout):
        currentValue = self._currentValue(setting)
        originalValue = self._originalValue(setting, currentValue)
        widget = self._makeWidget(setting, currentValue, settingMetadata)
        if widget is None:
            return

        keyText = str(setting.key)
        row = ParameterRow(
            setting.name,
            widget,
            authored=not valuesEqual(currentValue, originalValue),
            labelWidth=self._settingLabelWidth,
            parent=self._formContainer)
        row.labelWidget.setToolTip(
            "{}\nDefault: {}".format(keyText, self._valueText(setting.defValue)))
        self._applyControlHeight(widget)
        row.labelWidget.setMinimumHeight(widget.minimumHeight())
        row.labelWidget.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
        row.resetRequested.connect(
            lambda s=setting, w=widget, r=row: self._resetSetting(s, w, r))
        layout.addRow(row.labelWidget, widget)
        self._rowsByWidget[widget] = row

    def _controlHeightHint(self):
        if self._controlHeight is None:
            spinBox = QtWidgets.QDoubleSpinBox()
            self._controlHeight = spinBox.sizeHint().height()
            spinBox.deleteLater()
        return self._controlHeight

    def _applyControlHeight(self, widget):
        height = max(self._controlHeightHint(), widget.sizeHint().height())
        widget.setMinimumHeight(height)
        widget.setMaximumHeight(height)

    def _makeWidget(self, setting, value, settingMetadata):
        settingType = setting.type
        if settingType == UsdImagingGL.RendererSettingType.FLAG:
            widget = BoolControl(value=bool(value))
            widget.setSizePolicy(
                QtWidgets.QSizePolicy.Fixed,
                QtWidgets.QSizePolicy.Fixed)
            widget.toggled.connect(
                lambda checked, s=setting, w=widget:
                    self._setSetting(s, bool(checked), w))
            return widget

        if settingType == UsdImagingGL.RendererSettingType.INT:
            widget = IntControl(
                value=int(value),
                valueRange=_INT_RANGE,
                spinWidth=_INT_SPINBOX_WIDTH)
            widget.setSizePolicy(
                QtWidgets.QSizePolicy.Expanding,
                QtWidgets.QSizePolicy.Fixed)
            widget.valueChanged.connect(
                lambda nextValue, s=setting, w=widget:
                    self._setSetting(s, int(nextValue), w))
            return widget

        if settingType == UsdImagingGL.RendererSettingType.FLOAT:
            widget = FloatControl(
                value=float(value),
                sliderRange=(0.0, 1.0),
                spinRange=_FLOAT_RANGE,
                step=0.01,
                decimals=6,
                spinWidth=_FLOAT_SPINBOX_WIDTH,
                showSlider=False,
                alignRight=True)
            widget.setSizePolicy(
                QtWidgets.QSizePolicy.Expanding,
                QtWidgets.QSizePolicy.Fixed)
            widget.valueChanged.connect(
                lambda nextValue, s=setting, w=widget:
                    self._setSetting(s, float(nextValue), w))
            return widget

        if settingType == UsdImagingGL.RendererSettingType.STRING:
            allowedTokens = settingMetadata.get("allowedTokens")
            if allowedTokens:
                widget = NoWheelComboBox()
                widget.setMinimumWidth(_COMBO_BOX_WIDTH)
                widget.setSizePolicy(
                    QtWidgets.QSizePolicy.Expanding,
                    QtWidgets.QSizePolicy.Fixed)
                valueText = str(value)
                tokenTexts = [str(token) for token in allowedTokens]
                if valueText and valueText not in tokenTexts:
                    tokenTexts.insert(0, valueText)
                widget.addItems(tokenTexts)
                index = widget.findText(valueText)
                if index >= 0:
                    widget.setCurrentIndex(index)
                widget.currentIndexChanged.connect(
                    lambda _index, s=setting, w=widget:
                        self._setSetting(s, w.currentText(), w))
                return widget

            widget = QtWidgets.QLineEdit(str(value))
            widget.setMinimumWidth(_STRING_WIDTH)
            widget.setSizePolicy(
                QtWidgets.QSizePolicy.Expanding,
                QtWidgets.QSizePolicy.Fixed)
            widget.editingFinished.connect(
                lambda s=setting, w=widget:
                    self._setSetting(s, w.text(), w))
            return widget

        return None

    def _setSetting(self, setting, value, widget):
        if self._updating:
            return

        stageView = self._stageView()
        if not stageView:
            return

        try:
            stageView.SetRendererSetting(str(setting.key), value)
            row = self._rowsByWidget.get(widget)
            if row:
                row.setAuthored(not valuesEqual(
                    value,
                    self._originalValue(setting, value)))
        except Exception as err:
            print("RenderLab Render Settings error: {}".format(err))

    def _resetSetting(self, setting, widget, row):
        value = self._originalValue(setting, self._currentValue(setting))
        try:
            self._updating = True
            self._setWidgetValue(widget, value)
            self._updating = False
            stageView = self._stageView()
            if stageView:
                stageView.SetRendererSetting(str(setting.key), value)
            row.setAuthored(False)
        except Exception as err:
            self._updating = False
            print("RenderLab Render Settings error: {}".format(err))

    def _currentValue(self, setting):
        stageView = self._stageView()
        if not stageView:
            return setting.defValue
        value = stageView.GetRendererSetting(setting.key)
        if value is None:
            return setting.defValue
        return value

    def _originalValue(self, setting, fallbackValue):
        key = (self._rendererId(), str(setting.key))
        if key not in self._originalValues:
            self._originalValues[key] = fallbackValue
        return self._originalValues[key]

    def _setWidgetValue(self, widget, value):
        if isinstance(widget, QtWidgets.QLineEdit):
            widget.setText(str(value))
        elif isinstance(widget, QtWidgets.QComboBox):
            valueText = str(value)
            index = widget.findText(valueText)
            if index < 0:
                widget.insertItem(0, valueText)
                index = 0
            widget.setCurrentIndex(index)
        else:
            widget.setValue(value)

    def _valueText(self, value):
        return "" if value is None else str(value)
