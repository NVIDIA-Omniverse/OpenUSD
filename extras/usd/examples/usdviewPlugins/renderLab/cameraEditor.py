"""Camera controls for RenderLab."""

from pxr import Gf, Sdf, UsdGeom, Vt
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from .parameterWidgets import (
    FloatControl,
    NoWheelComboBox,
    ParameterRow,
    Vec2Control,
    Vec4Control,
    valuesEqual,
)


_FLOAT_SPINBOX_WIDTH = 104
_VEC4_SPINBOX_WIDTH = 70
_COMBO_BOX_WIDTH = 120
_LABEL_WIDTH_REFERENCE = "transmission_dispersion_abbe_number"
_LABEL_WIDTH_PADDING = 16
_PANE_LEFT_MARGIN = 8
_EDITOR_PANE_WIDTH = 730
_PANE_TITLE_STYLE = "font-weight: bold; color: #999999;"
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
_CAMERA_LABELS = (
    "Projection",
    "Focal Length",
    "Horizontal Aperture",
    "Vertical Aperture",
    "Horizontal Aperture Offset",
    "Vertical Aperture Offset",
    "Clipping Range",
    "Clipping Planes",
    "Focus Distance",
    "F-Stop",
    "Stereo Role",
    "Shutter Open",
    "Shutter Close",
    "Exposure",
    "Exposure ISO",
    "Exposure Time",
    "Exposure F-Stop",
    "Exposure Responsivity",
)


class Vec4ArrayControl(QtWidgets.QWidget):
    """Edits a float4[] value as a compact list of Vec4 rows."""

    valueChanged = QtCore.Signal(object)

    def __init__(
            self,
            value=None,
            valueRange=(-1e6, 1e6),
            step=0.01,
            decimals=4,
            spinWidth=None,
            parent=None):
        super().__init__(parent)
        self._valueRange = valueRange
        self._step = step
        self._decimals = decimals
        self._spinWidth = spinWidth
        self._rows = []
        self._updating = False

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)

        self._rowsLayout = QtWidgets.QVBoxLayout()
        self._rowsLayout.setContentsMargins(0, 0, 0, 0)
        self._rowsLayout.setSpacing(4)
        layout.addLayout(self._rowsLayout)

        buttons = QtWidgets.QHBoxLayout()
        buttons.setContentsMargins(0, 0, 0, 0)
        buttons.addStretch(1)
        self._addButton = QtWidgets.QPushButton("+")
        self._addButton.setFixedWidth(28)
        self._addButton.setToolTip("Add clipping plane")
        self._addButton.clicked.connect(self._addDefaultRow)
        buttons.addWidget(self._addButton)
        layout.addLayout(buttons)

        self.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding,
            QtWidgets.QSizePolicy.Maximum)
        self.setValue(() if value is None else value)

    def value(self):
        return tuple(control.value() for _rowWidget, control in self._rows)

    def setValue(self, value):
        self._updating = True
        self._clearRows()
        for plane in value:
            self._addRow(plane)
        self._updating = False

    def _addDefaultRow(self):
        self._addRow((0.0, 0.0, 0.0, 0.0))
        self._emitValueChanged()

    def _addRow(self, plane):
        rowWidget = QtWidgets.QWidget()
        rowLayout = QtWidgets.QHBoxLayout(rowWidget)
        rowLayout.setContentsMargins(0, 0, 0, 0)
        rowLayout.setSpacing(4)

        control = Vec4Control(
            value=plane,
            valueRange=self._valueRange,
            step=self._step,
            decimals=self._decimals,
            spinWidth=self._spinWidth,
            alignRight=True)
        control.valueChanged.connect(self._emitValueChanged)
        rowLayout.addWidget(control, 1)

        removeButton = QtWidgets.QPushButton("-")
        removeButton.setFixedWidth(28)
        removeButton.setToolTip("Remove clipping plane")
        removeButton.clicked.connect(
            lambda _checked=False, widget=rowWidget: self._removeRow(widget))
        rowLayout.addWidget(removeButton)

        self._rowsLayout.addWidget(rowWidget)
        self._rows.append((rowWidget, control))

    def _removeRow(self, rowWidget):
        for index, (widget, _control) in enumerate(self._rows):
            if widget is rowWidget:
                self._rows.pop(index)
                widget.setParent(None)
                widget.deleteLater()
                self._emitValueChanged()
                return

    def _clearRows(self):
        while self._rows:
            widget, _control = self._rows.pop()
            widget.setParent(None)
            widget.deleteLater()

    def _emitValueChanged(self, *_args):
        if not self._updating:
            self.valueChanged.emit(self.value())


class CameraEditor(QtWidgets.QWidget):

    def __init__(self, usdviewApi, parent=None):
        super().__init__(parent)
        self._api = usdviewApi
        self._updating = False
        self._currentPath = None
        self._controlHeight = None
        self._labelWidth = 0
        self._groupLayouts = {}

        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(_PANE_LEFT_MARGIN, 8, 8, 8)
        root.setSpacing(15)

        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        left = QtWidgets.QWidget()
        left.setFixedWidth(_EDITOR_PANE_WIDTH)
        leftLayout = QtWidgets.QVBoxLayout(left)
        leftLayout.setContentsMargins(0, 0, 4, 0)
        leftLayout.setSpacing(0)

        titleRow = QtWidgets.QHBoxLayout()
        titleRow.setContentsMargins(_GROUP_CONTENT_LEFT_MARGIN, 0, 0, 0)
        titleLabel = QtWidgets.QLabel("Cameras")
        titleLabel.setStyleSheet(_PANE_TITLE_STYLE)
        titleRow.addWidget(titleLabel, 1)
        leftLayout.addLayout(titleRow)

        self._headerRow = QtWidgets.QHBoxLayout()
        self._headerRow.setContentsMargins(
            _GROUP_CONTENT_LEFT_MARGIN, 0, 0, 0)
        self._cameraManipulatorCheck = QtWidgets.QCheckBox("Interactive")
        self._cameraManipulatorCheck.setToolTip(
            "Alt-drag in the viewport edits the selected camera.")
        self._cameraManipulatorCheck.toggled.connect(
            self._onCameraManipulatorToggled)
        self._headerRow.addStretch(1)
        self._headerRow.addWidget(self._cameraManipulatorCheck)
        leftLayout.addLayout(self._headerRow)
        leftLayout.addSpacing(5)

        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        self._scroll.verticalScrollBar().rangeChanged.connect(
            lambda _minimum, _maximum: self._syncHeaderRightMargin())
        leftLayout.addWidget(self._scroll, 1)
        splitter.addWidget(left)

        listPane = QtWidgets.QWidget()
        listLayout = QtWidgets.QVBoxLayout(listPane)
        listLayout.setContentsMargins(4, 0, 0, 0)

        self._cameraList = QtWidgets.QListWidget()
        self._cameraList.currentItemChanged.connect(self._onCameraChanged)
        self._cameraList.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self._cameraList.customContextMenuRequested.connect(
            self._showCameraContextMenu)
        listLayout.addWidget(self._cameraList, 1)
        splitter.addWidget(listPane)

        splitter.setSizes([_EDITOR_PANE_WIDTH, 260])
        splitter.setStretchFactor(0, 1)
        root.addWidget(splitter, 1)

        self._newForm()
        self.refresh()

    def activateEditor(self):
        self.refresh()
        self._cameraList.setFocus()

    def refresh(self):
        currentPath = self._currentPath
        cameraPrim = self._api.cameraPrim
        if cameraPrim and cameraPrim.IsValid():
            currentPath = str(cameraPrim.GetPath())

        self._cameraList.blockSignals(True)
        self._cameraList.clear()
        stage = self._api.stage
        if stage:
            for prim in stage.Traverse():
                if prim.IsA(UsdGeom.Camera):
                    item = QtWidgets.QListWidgetItem(str(prim.GetPath()))
                    item.setData(QtCore.Qt.UserRole, str(prim.GetPath()))
                    self._cameraList.addItem(item)
        self._cameraList.blockSignals(False)

        if not self._selectPath(currentPath) and self._cameraList.count():
            self._cameraList.setCurrentRow(0)
        elif self._cameraList.count() == 0:
            self._showCamera(None)

    def _selectPath(self, pathText):
        if not pathText:
            return False
        for row in range(self._cameraList.count()):
            item = self._cameraList.item(row)
            if item.data(QtCore.Qt.UserRole) == pathText:
                self._cameraList.setCurrentRow(row)
                return True
        return False

    def _showCameraContextMenu(self, position):
        item = self._cameraList.itemAt(position)
        if not item:
            return

        self._cameraList.setCurrentItem(item)
        menu = QtWidgets.QMenu(self._cameraList)
        useAction = menu.addAction("Use as View Camera")
        useAction.triggered.connect(self._useAsViewCamera)
        menu.exec_(self._cameraList.mapToGlobal(position))

    def _onCameraChanged(self, item, _previous):
        if not item:
            self._showCamera(None)
            return
        self._currentPath = item.data(QtCore.Qt.UserRole)
        stage = self._api.stage
        prim = stage.GetPrimAtPath(Sdf.Path(self._currentPath)) if stage else None
        self._showCamera(prim)
        if self.cameraManipulatorEnabled():
            self._activateInteractiveCamera()

    def _showCamera(self, prim):
        self._newForm()
        if not prim or not prim.IsValid():
            return

        camera = UsdGeom.Camera(prim)
        frustumLayout = self._addCollapsibleGroup("Viewing Frustum")
        dofLayout = self._addCollapsibleGroup("Depth of Field")
        stereoLayout = self._addCollapsibleGroup("Stereo")
        shutterLayout = self._addCollapsibleGroup("Shutter")
        exposureLayout = self._addCollapsibleGroup("Exposure")

        self._addTokenAttr(
            "Projection",
            camera.GetProjectionAttr,
            camera.CreateProjectionAttr,
            UsdGeom.Tokens.perspective,
            (UsdGeom.Tokens.perspective, UsdGeom.Tokens.orthographic),
            frustumLayout)

        self._addFloatAttr(
            "Focal Length",
            camera.GetFocalLengthAttr,
            camera.CreateFocalLengthAttr,
            50.0,
            0.001,
            100000.0,
            frustumLayout)
        self._addFloatAttr(
            "Horizontal Aperture",
            camera.GetHorizontalApertureAttr,
            camera.CreateHorizontalApertureAttr,
            20.955,
            0.001,
            100000.0,
            frustumLayout)
        self._addFloatAttr(
            "Vertical Aperture",
            camera.GetVerticalApertureAttr,
            camera.CreateVerticalApertureAttr,
            15.2908,
            0.001,
            100000.0,
            frustumLayout)
        self._addFloatAttr(
            "Horizontal Aperture Offset",
            camera.GetHorizontalApertureOffsetAttr,
            camera.CreateHorizontalApertureOffsetAttr,
            0.0,
            -100000.0,
            100000.0,
            frustumLayout)
        self._addFloatAttr(
            "Vertical Aperture Offset",
            camera.GetVerticalApertureOffsetAttr,
            camera.CreateVerticalApertureOffsetAttr,
            0.0,
            -100000.0,
            100000.0,
            frustumLayout)
        self._addClippingRange(camera, frustumLayout)
        self._addClippingPlanes(camera, frustumLayout)

        self._addFloatAttr(
            "Focus Distance",
            camera.GetFocusDistanceAttr,
            camera.CreateFocusDistanceAttr,
            0.0,
            0.0,
            1000000.0,
            dofLayout)
        self._addFloatAttr(
            "F-Stop",
            camera.GetFStopAttr,
            camera.CreateFStopAttr,
            0.0,
            0.0,
            256.0,
            dofLayout)
        self._addTokenAttr(
            "Stereo Role",
            camera.GetStereoRoleAttr,
            camera.CreateStereoRoleAttr,
            UsdGeom.Tokens.mono,
            (UsdGeom.Tokens.mono, UsdGeom.Tokens.left, UsdGeom.Tokens.right),
            stereoLayout)
        self._addFloatAttr(
            "Shutter Open",
            camera.GetShutterOpenAttr,
            camera.CreateShutterOpenAttr,
            0.0,
            -1000000.0,
            1000000.0,
            shutterLayout,
            step=0.01,
            decimals=6)
        self._addFloatAttr(
            "Shutter Close",
            camera.GetShutterCloseAttr,
            camera.CreateShutterCloseAttr,
            0.0,
            -1000000.0,
            1000000.0,
            shutterLayout,
            step=0.01,
            decimals=6)
        self._addFloatAttr(
            "Exposure",
            camera.GetExposureAttr,
            camera.CreateExposureAttr,
            0.0,
            -1000000.0,
            1000000.0,
            exposureLayout,
            step=0.01,
            decimals=6)
        self._addFloatAttr(
            "Exposure ISO",
            camera.GetExposureIsoAttr,
            camera.CreateExposureIsoAttr,
            100.0,
            0.0,
            1000000.0,
            exposureLayout)
        self._addFloatAttr(
            "Exposure Time",
            camera.GetExposureTimeAttr,
            camera.CreateExposureTimeAttr,
            1.0,
            0.0,
            1000000.0,
            exposureLayout,
            step=0.01,
            decimals=6)
        self._addFloatAttr(
            "Exposure F-Stop",
            camera.GetExposureFStopAttr,
            camera.CreateExposureFStopAttr,
            1.0,
            0.0,
            256.0,
            exposureLayout)
        self._addFloatAttr(
            "Exposure Responsivity",
            camera.GetExposureResponsivityAttr,
            camera.CreateExposureResponsivityAttr,
            1.0,
            0.0,
            1000000.0,
            exposureLayout)

    def _newForm(self):
        self._formContainer = QtWidgets.QWidget()
        self._formLayout = QtWidgets.QFormLayout(self._formContainer)
        self._formLayout.setFieldGrowthPolicy(
            QtWidgets.QFormLayout.ExpandingFieldsGrow)
        self._formLayout.setLabelAlignment(QtCore.Qt.AlignLeft)
        self._formLayout.setContentsMargins(11, 0, 11, 11)
        self._formLayout.setHorizontalSpacing(12)
        self._formLayout.setVerticalSpacing(5)
        self._labelWidth = self._computeLabelWidth(_CAMERA_LABELS)
        self._groupLayouts = {}
        self._scroll.setWidget(self._formContainer)
        self._syncHeaderRightMargin()
        QtCore.QTimer.singleShot(0, self._syncHeaderRightMargin)

    def _syncHeaderRightMargin(self):
        if not hasattr(self, "_headerRow"):
            return

        rightMargin = 0
        if hasattr(self, "_formLayout") and self._formLayout is not None:
            rightMargin += self._formLayout.contentsMargins().right()
        if hasattr(self, "_scroll") and self._scroll is not None:
            scrollBar = self._scroll.verticalScrollBar()
            if scrollBar and (
                    scrollBar.isVisible() or
                    scrollBar.maximum() > scrollBar.minimum()):
                rightMargin += scrollBar.sizeHint().width()

        self._headerRow.setContentsMargins(
            _GROUP_CONTENT_LEFT_MARGIN, 0, rightMargin, 0)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._syncHeaderRightMargin()

    def _addFloatAttr(
            self, label, getAttrFn, createAttrFn, defaultValue, minimum, maximum,
            layout, step=0.1, decimals=4):
        value = getAttrFn().Get()
        control = FloatControl(
            value=float(defaultValue if value is None else value),
            sliderRange=(minimum, maximum),
            spinRange=(minimum, maximum),
            step=step,
            decimals=decimals,
            spinWidth=_FLOAT_SPINBOX_WIDTH,
            showSlider=False,
            alignRight=True)
        originalValue = control.value()
        row = self._addParameterRow(label, control, layout)
        row.resetRequested.connect(
            lambda c=control, v=originalValue, r=row:
                self._resetAttr(createAttrFn, c, v, r, label))
        control.valueChanged.connect(
            lambda value, r=row, v=originalValue:
                self._setAttrValue(createAttrFn, float(value), label, r, v))

    def _addTokenAttr(
            self, label, getAttrFn, createAttrFn, defaultValue, values, layout):
        combo = NoWheelComboBox()
        combo.setMinimumWidth(_COMBO_BOX_WIDTH)
        combo.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding,
            QtWidgets.QSizePolicy.Fixed)
        for value in values:
            combo.addItem(str(value), value)

        self._updating = True
        originalValue = getAttrFn().Get() or defaultValue
        self._setComboValue(combo, originalValue)
        self._updating = False
        row = self._addParameterRow(label, combo, layout)
        row.resetRequested.connect(
            lambda c=combo, v=originalValue, r=row, f=createAttrFn, l=label:
                self._resetAttr(f, c, v, r, l))
        combo.currentIndexChanged.connect(
            lambda _index, c=combo, f=createAttrFn, r=row, v=originalValue, l=label:
                self._setTokenAttr(c, f, l, r, v))

    def _addClippingRange(self, camera, layout):
        value = camera.GetClippingRangeAttr().Get()
        if value is None:
            value = Gf.Vec2f(1.0, 1000000.0)
        control = Vec2Control(
            value=value,
            valueRange=(0.0, 100000000.0),
            step=0.1,
            decimals=4,
            spinWidth=_FLOAT_SPINBOX_WIDTH,
            alignRight=True)
        originalValue = tuple(control.value())
        row = self._addParameterRow("Clipping Range", control, layout)
        row.resetRequested.connect(
            lambda c=control, v=originalValue, r=row:
                self._resetAttr(
                    camera.CreateClippingRangeAttr,
                    c,
                    Gf.Vec2f(*v),
                    r,
                    "Clipping Range"))
        control.valueChanged.connect(
            lambda vec, r=row, v=originalValue: self._setAttrValue(
                camera.CreateClippingRangeAttr,
                Gf.Vec2f(*vec),
                "Clipping Range",
                r,
                v))

    def _addClippingPlanes(self, camera, layout):
        value = camera.GetClippingPlanesAttr().Get()
        if value is None:
            value = Vt.Vec4fArray()
        originalValue = self._clippingPlanesToTuple(value)

        control = Vec4ArrayControl(
            value=originalValue,
            valueRange=(-100000000.0, 100000000.0),
            step=0.1,
            decimals=4,
            spinWidth=_VEC4_SPINBOX_WIDTH)
        row = self._addParameterRow(
            "Clipping Planes", control, layout, fixedHeight=False)
        row.resetRequested.connect(
            lambda c=control, v=originalValue, r=row:
                self._resetClippingPlanes(camera, c, v, r))
        control.valueChanged.connect(
            lambda _planes, c=control, v=originalValue, r=row:
                self._setClippingPlanes(camera, c, v, r))

    def _setTokenAttr(self, combo, createAttrFn, label, row, originalValue):
        if self._updating:
            return
        value = combo.itemData(combo.currentIndex())
        self._setAttrValue(
            createAttrFn,
            value,
            label,
            row,
            originalValue)

    def _setClippingPlanes(self, camera, control, originalValue, row):
        if self._updating:
            return
        planes = control.value()
        self._setAttrValue(
            camera.CreateClippingPlanesAttr,
            self._clippingPlanesToArray(planes),
            "Clipping Planes",
            row,
            originalValue)

    def _resetClippingPlanes(self, camera, control, originalValue, row):
        stage = self._api.stage
        if stage:
            stage.SetEditTarget(stage.GetSessionLayer())
        try:
            camera.CreateClippingPlanesAttr().Clear()
            self._updating = True
            control.setValue(originalValue)
            self._updating = False
            row.setAuthored(False)
            self._api.UpdateViewport()
        except Exception as err:
            self._updating = False
            print("RenderLab Camera error: {}".format(err))

    def _setAttrValue(
            self, createAttrFn, value, label, row=None, originalValue=None):
        if self._updating:
            return
        stage = self._api.stage
        if stage:
            stage.SetEditTarget(stage.GetSessionLayer())
        try:
            createAttrFn().Set(value)
            if row is not None and originalValue is not None:
                row.setAuthored(not valuesEqual(value, originalValue))
            elif row is not None:
                row.setAuthored(True)
            self._api.UpdateViewport()
        except Exception as err:
            print("RenderLab Camera error: {}".format(err))

    def _resetAttr(self, createAttrFn, control, originalValue, row, label):
        stage = self._api.stage
        if stage:
            stage.SetEditTarget(stage.GetSessionLayer())
        try:
            createAttrFn().Clear()
            self._updating = True
            if isinstance(control, QtWidgets.QComboBox):
                self._setComboValue(control, originalValue)
            elif isinstance(control, QtWidgets.QLineEdit):
                control.setText(str(originalValue))
            else:
                control.setValue(originalValue)
            self._updating = False
            row.setAuthored(False)
            self._api.UpdateViewport()
        except Exception as err:
            self._updating = False
            print("RenderLab Camera error: {}".format(err))

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
        self._formLayout.addRow(button)
        self._formLayout.addRow(content)
        self._groupLayouts[category] = layout
        return layout

    def _addParameterRow(self, label, widget, layout, fixedHeight=True):
        row = ParameterRow(
            label,
            widget,
            authored=False,
            labelWidth=self._labelWidth,
            parent=self._formContainer)
        if fixedHeight:
            self._applyControlHeight(widget)
            row.labelWidget.setMinimumHeight(widget.minimumHeight())
            row.labelWidget.setAlignment(
                QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
        else:
            row.labelWidget.setMinimumHeight(self._controlHeightHint())
            row.labelWidget.setAlignment(
                QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop)
        layout.addRow(row.labelWidget, widget)
        return row

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

    def _computeLabelWidth(self, labels):
        referenceLabels = list(labels) + [_LABEL_WIDTH_REFERENCE]
        normalMetrics = QtGui.QFontMetrics(self.font())
        italicFont = QtGui.QFont(self.font())
        italicFont.setItalic(True)
        italicMetrics = QtGui.QFontMetrics(italicFont)
        return max(
            max(normalMetrics.horizontalAdvance(label) for label in referenceLabels),
            max(italicMetrics.horizontalAdvance(label) for label in referenceLabels)
        ) + _LABEL_WIDTH_PADDING

    def _useAsViewCamera(self):
        prim = self._currentPrim()
        if not prim:
            return
        self._api.dataModel.viewSettings.cameraPrim = prim
        self._api.UpdateViewport()

    def currentPath(self):
        return self._currentPath

    def cameraManipulatorEnabled(self):
        return self._cameraManipulatorCheck.isChecked()

    def _onCameraManipulatorToggled(self, enabled):
        if enabled and not self._currentPrim():
            self._cameraManipulatorCheck.setChecked(False)
            return
        if enabled:
            self._activateInteractiveCamera()
            try:
                self._api.PrintStatus(
                    "RenderLab: camera interactive editing enabled")
            except Exception:
                pass

    def _activateInteractiveCamera(self):
        manipulator = getattr(self._api, "_renderLabCameraManipulator", None)
        if manipulator is not None:
            adoptCurrentView = getattr(manipulator, "AdoptCurrentView", None)
            if adoptCurrentView is not None and adoptCurrentView(self._currentPath):
                return
        self._useAsViewCamera()

    def _currentPrim(self):
        if not self._currentPath or not self._api.stage:
            return None
        prim = self._api.stage.GetPrimAtPath(Sdf.Path(self._currentPath))
        return prim if prim and prim.IsValid() else None

    def _setComboValue(self, combo, value):
        index = combo.findData(value)
        if index < 0:
            index = combo.findText(str(value))
        if index >= 0:
            combo.setCurrentIndex(index)

    def _clippingPlanesToTuple(self, value):
        return tuple(tuple(float(component) for component in plane)
                     for plane in value)

    def _clippingPlanesToArray(self, value):
        return Vt.Vec4fArray([Gf.Vec4f(*plane) for plane in value])
