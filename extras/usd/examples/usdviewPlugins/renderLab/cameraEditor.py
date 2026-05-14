"""Camera controls for RenderLab."""

from pxr import Gf, Sdf, UsdGeom
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from .parameterWidgets import FloatControl, ParameterRow, Vec2Control, valuesEqual


_FLOAT_SPINBOX_WIDTH = 104
_COMBO_BOX_WIDTH = 120
_LABEL_WIDTH_PADDING = 16
_PANE_LEFT_MARGIN = 20
_PANE_TITLE_STYLE = "font-weight: bold; color: #999999;"
_CAMERA_LABELS = (
    "Projection",
    "Focal Length",
    "Focus Distance",
    "F-Stop",
    "Horizontal Aperture",
    "Vertical Aperture",
    "Clipping Range",
)


class CameraEditor(QtWidgets.QWidget):

    def __init__(self, usdviewApi, parent=None):
        super().__init__(parent)
        self._api = usdviewApi
        self._updating = False
        self._currentPath = None
        self._controlHeight = None
        self._labelWidth = 0

        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(_PANE_LEFT_MARGIN, 8, 8, 8)
        root.setSpacing(15)

        titleRow = QtWidgets.QHBoxLayout()
        titleRow.setContentsMargins(0, 0, 0, 0)
        titleLabel = QtWidgets.QLabel("Cameras")
        titleLabel.setStyleSheet(_PANE_TITLE_STYLE)
        titleRow.addWidget(titleLabel, 1)
        root.addLayout(titleRow)

        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        self._cameraList = QtWidgets.QListWidget()
        self._cameraList.currentItemChanged.connect(self._onCameraChanged)
        self._cameraList.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self._cameraList.customContextMenuRequested.connect(
            self._showCameraContextMenu)
        splitter.addWidget(self._cameraList)

        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        splitter.addWidget(self._scroll)
        splitter.setSizes([260, 560])
        splitter.setStretchFactor(1, 1)
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

    def _showCamera(self, prim):
        self._newForm()
        if not prim or not prim.IsValid():
            return

        camera = UsdGeom.Camera(prim)

        projection = QtWidgets.QComboBox()
        projection.setFixedWidth(_COMBO_BOX_WIDTH)
        for value in (UsdGeom.Tokens.perspective, UsdGeom.Tokens.orthographic):
            projection.addItem(str(value), value)

        self._updating = True
        originalProjection = (
            camera.GetProjectionAttr().Get() or UsdGeom.Tokens.perspective)
        self._setComboValue(projection, originalProjection)
        self._updating = False
        projectionRow = self._addParameterRow("Projection", projection)
        projectionRow.resetRequested.connect(
            lambda c=projection, v=originalProjection, r=projectionRow:
                self._resetAttr(camera.CreateProjectionAttr, c, v, r, "Projection"))
        projection.currentIndexChanged.connect(
            lambda _=None, c=projection, v=originalProjection, r=projectionRow:
                self._setProjection(c, v, r))

        self._addFloatAttr(
            "Focal Length",
            camera.GetFocalLengthAttr,
            camera.CreateFocalLengthAttr,
            50.0,
            0.001,
            100000.0)
        self._addFloatAttr(
            "Focus Distance",
            camera.GetFocusDistanceAttr,
            camera.CreateFocusDistanceAttr,
            0.0,
            0.0,
            1000000.0)
        self._addFloatAttr(
            "F-Stop",
            camera.GetFStopAttr,
            camera.CreateFStopAttr,
            0.0,
            0.0,
            256.0)
        self._addFloatAttr(
            "Horizontal Aperture",
            camera.GetHorizontalApertureAttr,
            camera.CreateHorizontalApertureAttr,
            20.955,
            0.001,
            100000.0)
        self._addFloatAttr(
            "Vertical Aperture",
            camera.GetVerticalApertureAttr,
            camera.CreateVerticalApertureAttr,
            15.2908,
            0.001,
            100000.0)
        self._addClippingRange(camera)

    def _newForm(self):
        self._formContainer = QtWidgets.QWidget()
        self._formLayout = QtWidgets.QFormLayout(self._formContainer)
        self._formLayout.setFieldGrowthPolicy(
            QtWidgets.QFormLayout.ExpandingFieldsGrow)
        self._formLayout.setLabelAlignment(QtCore.Qt.AlignLeft)
        self._formLayout.setHorizontalSpacing(12)
        self._formLayout.setVerticalSpacing(5)
        self._labelWidth = self._computeLabelWidth(_CAMERA_LABELS)
        self._scroll.setWidget(self._formContainer)

    def _addFloatAttr(
            self, label, getAttrFn, createAttrFn, defaultValue, minimum, maximum):
        value = getAttrFn().Get()
        control = FloatControl(
            value=float(defaultValue if value is None else value),
            sliderRange=(minimum, maximum),
            spinRange=(minimum, maximum),
            step=0.1,
            decimals=4,
            spinWidth=_FLOAT_SPINBOX_WIDTH,
            alignRight=True,
            showSlider=False)
        originalValue = control.value()
        row = self._addParameterRow(label, control)
        row.resetRequested.connect(
            lambda c=control, v=originalValue, r=row:
                self._resetAttr(createAttrFn, c, v, r, label))
        control.valueChanged.connect(
            lambda value, r=row, v=originalValue:
                self._setAttrValue(createAttrFn, float(value), label, r, v))

    def _addClippingRange(self, camera):
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
        row = self._addParameterRow("Clipping Range", control)
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

    def _setProjection(self, combo, originalValue, row):
        if self._updating:
            return
        prim = self._currentPrim()
        if not prim:
            return
        value = combo.itemData(combo.currentIndex())
        camera = UsdGeom.Camera(prim)
        self._setAttrValue(
            camera.CreateProjectionAttr,
            value,
            "Projection",
            row,
            originalValue)

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
            else:
                control.setValue(originalValue)
            self._updating = False
            row.setAuthored(False)
            self._api.UpdateViewport()
        except Exception as err:
            self._updating = False
            print("RenderLab Camera error: {}".format(err))

    def _addParameterRow(self, label, widget):
        row = ParameterRow(
            label,
            widget,
            authored=False,
            labelWidth=self._labelWidth,
            parent=self._formContainer)
        self._applyControlHeight(widget)
        fieldWidget = self._rightAlignedWidget(widget)
        self._applyControlHeight(fieldWidget)
        row.labelWidget.setMinimumHeight(fieldWidget.minimumHeight())
        row.labelWidget.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
        self._formLayout.addRow(row.labelWidget, fieldWidget)
        return row

    def _rightAlignedWidget(self, widget):
        container = QtWidgets.QWidget()
        layout = QtWidgets.QHBoxLayout(container)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addStretch(1)
        layout.addWidget(widget)
        return container

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
        normalMetrics = QtGui.QFontMetrics(self.font())
        italicFont = QtGui.QFont(self.font())
        italicFont.setItalic(True)
        italicMetrics = QtGui.QFontMetrics(italicFont)
        return max(
            max(normalMetrics.horizontalAdvance(label) for label in labels),
            max(italicMetrics.horizontalAdvance(label) for label in labels)
        ) + _LABEL_WIDTH_PADDING

    def _useAsViewCamera(self):
        prim = self._currentPrim()
        if not prim:
            return
        self._api.dataModel.viewSettings.cameraPrim = prim
        self._api.UpdateViewport()

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
