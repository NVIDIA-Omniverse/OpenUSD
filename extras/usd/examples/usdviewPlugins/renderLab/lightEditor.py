"""Light controls for RenderLab."""

from pxr import Gf, Sdf, UsdLux
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from .parameterWidgets import (
    BoolControl,
    Color3Control,
    FloatControl,
    ParameterRow,
    valuesEqual,
)


_FLOAT_SPINBOX_WIDTH = 104
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
_BASE_LIGHT_LABELS = (
    "Intensity",
    "Exposure",
    "Color",
    "Normalize",
    "Color Temperature",
    "Temperature",
)


def _schemaObject(prim, schemaName):
    schemaType = getattr(UsdLux, schemaName, None)
    return schemaType(prim) if schemaType else None


def _isLightPrim(prim):
    try:
        if prim.HasAPI(UsdLux.LightAPI):
            return True
    except Exception:
        pass
    for schemaName in (
            "BoundableLightBase",
            "NonboundableLightBase",
            "DomeLight",
            "DistantLight",
            "RectLight",
            "SphereLight",
            "DiskLight",
            "CylinderLight"):
        schema = getattr(UsdLux, schemaName, None)
        if schema and prim.IsA(schema):
            return True
    return False


class LightEditor(QtWidgets.QWidget):

    def __init__(self, usdviewApi, parent=None):
        super().__init__(parent)
        self._api = usdviewApi
        self._currentPath = None
        self._updating = False
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
        titleLabel = QtWidgets.QLabel("Lights")
        titleLabel.setStyleSheet(_PANE_TITLE_STYLE)
        titleLabel.setContentsMargins(0, 15, 0, 0)
        titleRow.addWidget(titleLabel, 1)
        leftLayout.addLayout(titleRow)

        self._headerRow = QtWidgets.QHBoxLayout()
        self._headerRow.setContentsMargins(
            _GROUP_CONTENT_LEFT_MARGIN, 0, 0, 0)
        self._headerLabel = QtWidgets.QLabel("Select a light")
        self._headerLabel.setWordWrap(True)
        self._headerLabel.setAlignment(
            QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        self._headerRow.addWidget(self._headerLabel, 1)
        leftLayout.addLayout(self._headerRow)
        leftLayout.addSpacing(5)

        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        leftLayout.addWidget(self._scroll)

        splitter.addWidget(left)

        listPane = QtWidgets.QWidget()
        listLayout = QtWidgets.QVBoxLayout(listPane)
        listLayout.setContentsMargins(4, 0, 0, 0)

        self._lightList = QtWidgets.QListWidget()
        self._lightList.currentItemChanged.connect(self._onLightChanged)
        listLayout.addWidget(self._lightList, 1)
        splitter.addWidget(listPane)

        splitter.setSizes([_EDITOR_PANE_WIDTH, 260])
        splitter.setStretchFactor(0, 1)
        root.addWidget(splitter, 1)

        self._newForm()
        self.refresh()

    def activateEditor(self):
        self.refresh()
        self._lightList.setFocus()

    def refresh(self):
        currentPath = self._currentPath
        self._lightList.blockSignals(True)
        self._lightList.clear()
        stage = self._api.stage
        if stage:
            for prim in stage.Traverse():
                if _isLightPrim(prim):
                    item = QtWidgets.QListWidgetItem(str(prim.GetPath()))
                    item.setData(QtCore.Qt.UserRole, str(prim.GetPath()))
                    self._lightList.addItem(item)
        self._lightList.blockSignals(False)

        if not self._selectPath(currentPath) and self._lightList.count():
            self._lightList.setCurrentRow(0)
        elif self._lightList.count() == 0:
            self._showLight(None)

    def _selectPath(self, pathText):
        if not pathText:
            return False
        for row in range(self._lightList.count()):
            item = self._lightList.item(row)
            if item.data(QtCore.Qt.UserRole) == pathText:
                self._lightList.setCurrentRow(row)
                return True
        return False

    def _onLightChanged(self, item, _previous):
        if not item:
            self._showLight(None)
            return
        self._currentPath = item.data(QtCore.Qt.UserRole)
        self._showLight(self._currentPrim())

    def _showLight(self, prim):
        self._newForm()
        if not prim or not prim.IsValid():
            self._headerLabel.setText("Select a light")
            return

        light = UsdLux.LightAPI(prim)
        self._headerLabel.setText(
            "<span style='color:#999999;'>Type:</span> "
            "<b>{}</b>".format(prim.GetTypeName()))
        self._labelWidth = self._computeLabelWidth(self._labelsForPrim(prim))

        self._addFloatAttr(
            "Intensity",
            light.GetIntensityAttr,
            light.CreateIntensityAttr,
            1.0,
            0.0,
            100000000.0,
            1.0,
            light.GetIntensityAttr().GetDisplayGroup() or "Basic")
        self._addFloatAttr(
            "Exposure",
            light.GetExposureAttr,
            light.CreateExposureAttr,
            0.0,
            -32.0,
            32.0,
            0.1,
            light.GetExposureAttr().GetDisplayGroup() or "Basic")
        self._addColorAttr(
            "Color",
            light.GetColorAttr,
            light.CreateColorAttr,
            Gf.Vec3f(1.0, 1.0, 1.0),
            light.GetColorAttr().GetDisplayGroup() or "Basic")
        self._addBoolAttr(
            "Normalize",
            light.GetNormalizeAttr,
            light.CreateNormalizeAttr,
            False,
            light.GetNormalizeAttr().GetDisplayGroup() or "Advanced")
        self._addBoolAttr(
            "Color Temperature",
            light.GetEnableColorTemperatureAttr,
            light.CreateEnableColorTemperatureAttr,
            False,
            light.GetEnableColorTemperatureAttr().GetDisplayGroup() or "Basic")
        self._addFloatAttr(
            "Temperature",
            light.GetColorTemperatureAttr,
            light.CreateColorTemperatureAttr,
            6500.0,
            1000.0,
            20000.0,
            100.0,
            light.GetColorTemperatureAttr().GetDisplayGroup() or "Basic")

        self._addTypeSpecificRows(prim)
        self._syncHeaderRightMargin()
        QtCore.QTimer.singleShot(0, self._syncHeaderRightMargin)

    def _addTypeSpecificRows(self, prim):
        sphere = _schemaObject(prim, "SphereLight")
        if sphere:
            self._addFloatAttr(
                "Radius",
                sphere.GetRadiusAttr,
                sphere.CreateRadiusAttr,
                1.0,
                0.0,
                1000000.0,
                0.1,
                sphere.GetRadiusAttr().GetDisplayGroup() or "Geometry")

        disk = _schemaObject(prim, "DiskLight")
        if disk:
            self._addFloatAttr(
                "Radius",
                disk.GetRadiusAttr,
                disk.CreateRadiusAttr,
                1.0,
                0.0,
                1000000.0,
                0.1,
                disk.GetRadiusAttr().GetDisplayGroup() or "Geometry")

        cylinder = _schemaObject(prim, "CylinderLight")
        if cylinder:
            self._addFloatAttr(
                "Radius",
                cylinder.GetRadiusAttr,
                cylinder.CreateRadiusAttr,
                1.0,
                0.0,
                1000000.0,
                0.1,
                cylinder.GetRadiusAttr().GetDisplayGroup() or "Geometry")
            self._addFloatAttr(
                "Length",
                cylinder.GetLengthAttr,
                cylinder.CreateLengthAttr,
                1.0,
                0.0,
                1000000.0,
                0.1,
                cylinder.GetLengthAttr().GetDisplayGroup() or "Geometry")

        rect = _schemaObject(prim, "RectLight")
        portal = _schemaObject(prim, "PortalLight")
        rectLike = rect or portal
        if rectLike:
            self._addFloatAttr(
                "Width",
                rectLike.GetWidthAttr,
                rectLike.CreateWidthAttr,
                1.0,
                0.0,
                1000000.0,
                0.1,
                rectLike.GetWidthAttr().GetDisplayGroup() or "Geometry")
            self._addFloatAttr(
                "Height",
                rectLike.GetHeightAttr,
                rectLike.CreateHeightAttr,
                1.0,
                0.0,
                1000000.0,
                0.1,
                rectLike.GetHeightAttr().GetDisplayGroup() or "Geometry")

        distant = _schemaObject(prim, "DistantLight")
        if distant:
            self._addFloatAttr(
                "Angle",
                distant.GetAngleAttr,
                distant.CreateAngleAttr,
                0.53,
                0.0,
                180.0,
                0.1,
                distant.GetAngleAttr().GetDisplayGroup() or "Basic")

    def _newForm(self):
        self._formContainer = QtWidgets.QWidget()
        self._formLayout = QtWidgets.QFormLayout(self._formContainer)
        self._formLayout.setFieldGrowthPolicy(
            QtWidgets.QFormLayout.ExpandingFieldsGrow)
        self._formLayout.setLabelAlignment(QtCore.Qt.AlignLeft)
        self._formLayout.setContentsMargins(11, 0, 11, 11)
        self._formLayout.setHorizontalSpacing(12)
        self._formLayout.setVerticalSpacing(5)
        self._groupLayouts = {}
        self._scroll.setWidget(self._formContainer)
        self._syncHeaderRightMargin()

    def _syncHeaderRightMargin(self):
        if not hasattr(self, "_headerRow"):
            return

        rightMargin = 0
        if hasattr(self, "_formLayout") and self._formLayout is not None:
            rightMargin += self._formLayout.contentsMargins().right()
        if hasattr(self, "_scroll") and self._scroll is not None:
            scrollBar = self._scroll.verticalScrollBar()
            if scrollBar and scrollBar.isVisible():
                rightMargin += scrollBar.sizeHint().width()

        self._headerRow.setContentsMargins(
            _GROUP_CONTENT_LEFT_MARGIN, 0, rightMargin, 0)

    def _addFloatAttr(
            self, label, getAttrFn, createAttrFn, defaultValue, minimum,
            maximum, step, group):
        value = getAttrFn().Get()
        control = FloatControl(
            value=float(defaultValue if value is None else value),
            sliderRange=(minimum, maximum),
            spinRange=(minimum, maximum),
            step=step,
            decimals=4,
            spinWidth=_FLOAT_SPINBOX_WIDTH,
            showSlider=False,
            alignRight=True)
        originalValue = control.value()
        row = self._addParameterRow(label, control, group)
        row.resetRequested.connect(
            lambda c=control, v=originalValue, r=row:
                self._resetAttr(createAttrFn, c, v, r, label))
        control.valueChanged.connect(
            lambda value, r=row, v=originalValue:
                self._setAttrValue(createAttrFn, float(value), label, r, v))

    def _addBoolAttr(self, label, getAttrFn, createAttrFn, defaultValue, group):
        value = getAttrFn().Get()
        originalValue = bool(defaultValue if value is None else value)
        checkBox = BoolControl(value=originalValue)
        row = self._addParameterRow(label, checkBox, group)
        row.resetRequested.connect(
            lambda c=checkBox, v=originalValue, r=row:
                self._resetAttr(createAttrFn, c, v, r, label))
        checkBox.toggled.connect(
            lambda checked, r=row, v=originalValue:
                self._setAttrValue(createAttrFn, checked, label, r, v))

    def _addColorAttr(self, label, getAttrFn, createAttrFn, defaultValue, group):
        value = getAttrFn().Get()
        if value is None:
            value = defaultValue
        control = Color3Control(
            value=value,
            valueRange=(0.0, 100.0),
            step=0.01,
            decimals=4,
            spinWidth=_FLOAT_SPINBOX_WIDTH,
            dialogParent=self,
            dialogTitle=label)
        originalValue = tuple(control.value())
        row = self._addParameterRow(label, control, group)
        row.resetRequested.connect(
            lambda c=control, v=originalValue, r=row:
                self._resetAttr(
                    createAttrFn, c, Gf.Vec3f(*v), r, label))
        control.valueChanged.connect(
            lambda rgb, r=row, v=originalValue: self._setAttrValue(
                createAttrFn, Gf.Vec3f(*rgb), label, r, v))

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
            print("RenderLab Light error: {}".format(err))

    def _resetAttr(self, createAttrFn, control, originalValue, row, label):
        stage = self._api.stage
        if stage:
            stage.SetEditTarget(stage.GetSessionLayer())
        try:
            createAttrFn().Clear()
            self._updating = True
            control.setValue(originalValue)
            self._updating = False
            row.setAuthored(False)
            self._api.UpdateViewport()
        except Exception as err:
            self._updating = False
            print("RenderLab Light error: {}".format(err))

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
            self._syncHeaderRightMargin()
            QtCore.QTimer.singleShot(0, self._syncHeaderRightMargin)

        button.toggled.connect(toggled)
        self._formLayout.addRow(button)
        self._formLayout.addRow(content)
        self._groupLayouts[category] = layout
        return layout

    def _layoutForGroup(self, category):
        if not category:
            category = "Other"
        layout = self._groupLayouts.get(category)
        if layout is None:
            layout = self._addCollapsibleGroup(category)
        return layout

    def _addParameterRow(self, label, widget, group):
        row = ParameterRow(
            label,
            widget,
            authored=False,
            labelWidth=self._labelWidth,
            parent=self._formContainer)
        self._applyControlHeight(widget)
        row.labelWidget.setMinimumHeight(widget.minimumHeight())
        row.labelWidget.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
        self._layoutForGroup(group).addRow(row.labelWidget, widget)
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

    def _labelsForPrim(self, prim):
        labels = list(_BASE_LIGHT_LABELS)
        if _schemaObject(prim, "SphereLight"):
            labels.append("Radius")
        if _schemaObject(prim, "DiskLight"):
            labels.append("Radius")
        if _schemaObject(prim, "CylinderLight"):
            labels.extend(("Radius", "Length"))
        if _schemaObject(prim, "RectLight") or _schemaObject(prim, "PortalLight"):
            labels.extend(("Width", "Height"))
        if _schemaObject(prim, "DistantLight"):
            labels.append("Angle")
        return labels

    def _currentPrim(self):
        if not self._currentPath or not self._api.stage:
            return None
        prim = self._api.stage.GetPrimAtPath(Sdf.Path(self._currentPath))
        return prim if prim and prim.IsValid() else None
