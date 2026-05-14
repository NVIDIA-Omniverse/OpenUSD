"""Material Editor tab implementation for RenderLab."""

import fnmatch

from pxr.Usdviewq.qt import QtWidgets, QtCore, QtGui
from pxr import UsdShade, Sdf, Gf, Sdr

from .parameterWidgets import (
    BoolControl,
    Color3Control,
    FloatControl,
    IntControl,
    ParameterRow,
    Vec3Control,
)

# Material contexts exposed in the editor.  "default" is USD's universal
# render context, authored as outputs:surface.
_MATERIAL_CONTEXTS = (("mtlx", "mtlx"), ("default", ""))
_DEFAULT_MATERIAL_CONTEXT = "mtlx"

# Slider-friendly ranges for well-known float inputs.
_FLOAT_RANGES = {
    "anisotropy_rotation": (0.0, 6.283185)
}
_DEFAULT_FLOAT_RANGE = (0.0, 1.0)

_SLIDER_STEPS = 1000

_GROUP_HEADER_STYLE = (
    "QToolButton {"
    " font-weight: bold;"
    " border: none;"
    " border-radius: 3px;"
    " color: #999999;"
    " background-color: #2d2d2d;"
    " padding: 3px 6px;"
    " margin-top: 5px;"
    " text-align: left;"
    "}"
    "QToolButton:hover { background-color: #333; border-color: #777; }")
_LABEL_WIDTH_REFERENCE = "transmission_dispersion_abbe_number"
_LABEL_WIDTH_PADDING = 16
_MIN_EDITOR_PANE_WIDTH = 440
_MIN_EDITOR_CONTROL_WIDTH = 260
_MIN_MATERIAL_LIST_WIDTH = 120
_MAX_MATERIAL_LIST_WIDTH = 520
_MIN_WINDOW_WIDTH = 720
_FLOAT_SPINBOX_WIDTH = 104
_WINDOW_CHROME_WIDTH = 48
_GROUP_CONTENT_LEFT_MARGIN = 12
_SEPARATOR_STYLE = "background-color: #999999; border: 0;"
_HEADER_LABEL_STYLE = "font-weight: bold; color: #999999;"

_BASIC_COLOR_COLS = 8
_BASIC_COLOR_ROWS = 6
_BASIC_COLOR_MIN_VALUE = 64
_COLOR_SWATCH_WIDTH = 36
_COLOR_SWATCH_HEIGHT = 22


def _getFallbackFloatRange(inputName):
    normalizedName = inputName.lower()

    for pattern, valueRange in _FLOAT_RANGES.items():
        if normalizedName == pattern.lower() and len(valueRange) == 2:
            return valueRange

    for pattern, valueRange in _FLOAT_RANGES.items():
        if len(valueRange) == 2 and fnmatch.fnmatchcase(
                normalizedName, pattern.lower()):
            return valueRange

    return _DEFAULT_FLOAT_RANGE


def _getFloatRangeFromSdrProperty(sdrProp):
    if not sdrProp:
        return None

    metadata = sdrProp.GetMetadata()
    if not metadata:
        return None

    minValue = metadata.get("uisoftmin", metadata.get("uimin"))
    maxValue = metadata.get("uisoftmax", metadata.get("uimax"))
    if minValue is None or maxValue is None:
        return None

    try:
        return float(minValue), float(maxValue)
    except (TypeError, ValueError):
        return None


def _getFloatRange(inputName, sdrProp=None):
    sdrRange = _getFloatRangeFromSdrProperty(sdrProp)
    if sdrRange is not None:
        return sdrRange
    return _getFallbackFloatRange(inputName)


# ---------------------------------------------------------------------------
# Lazy input - wraps an unauthored input that is created on first write
# ---------------------------------------------------------------------------

class _LazyInput:
    """Shader input that does not yet exist on the prim.

    Calling Set() will create it via UsdShade.Shader.CreateInput().
    """

    def __init__(self, shader, name, sdfType):
        self._shader = shader
        self._name = name
        self._sdfType = sdfType
        self._input = None

    def GetBaseName(self):
        return self._name

    def Set(self, value):
        if self._input is None:
            self._input = self._shader.CreateInput(self._name, self._sdfType)
        self._input.Set(value)


# ---------------------------------------------------------------------------
# Material editor
# ---------------------------------------------------------------------------

class MaterialEditor(QtWidgets.QWidget):
    """RenderLab tab that lists stage materials and exposes shader inputs."""

    def __init__(self, usdviewApi, parent=None):
        super().__init__(parent)
        self._api = usdviewApi
        self._currentMaterial = None
        self._inputWidgets = []
        self._inputRows = {}
        self._inputLabelWidth = 0
        self._lastPrimPath = None
        self._shaderStack = []
        self._fitScheduled = False

        # Debounce: accumulate rapid edits and flush once the user pauses.
        self._pendingValues = {}
        self._flushTimer = QtCore.QTimer(self)
        self._flushTimer.setSingleShot(True)
        self._flushTimer.setInterval(50)
        self._flushTimer.timeout.connect(self._flushPendingValues)

        self._buildUI()
        self._refreshMaterials()

        self._pollTimer = QtCore.QTimer(self)
        self._pollTimer.timeout.connect(self._pollSelection)
        self._pollTimer.start(500)

    def refresh(self):
        self._refreshMaterials()

    # ---- layout -----------------------------------------------------------

    def _buildUI(self):
        self.setWindowTitle("Material Editor")
        self.setMinimumSize(_MIN_WINDOW_WIDTH, 480)
        self.resize(860, 900)
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose)

        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(20, 6, 6, 0)
        root.setSpacing(12)
        self._inputLabelWidth = self._computeInputLabelWidth(
            [_LABEL_WIDTH_REFERENCE])

        # toolbar
        toolbar = QtWidgets.QHBoxLayout()
        contextLabel = QtWidgets.QLabel("Material Context:")
        contextLabel.setStyleSheet(_HEADER_LABEL_STYLE)
        toolbar.addWidget(contextLabel)
        self._contextCombo = QtWidgets.QComboBox()
        self._contextCombo.setMinimumWidth(96)
        self._contextCombo.view().setMinimumWidth(96)
        for label, context in _MATERIAL_CONTEXTS:
            self._contextCombo.addItem(label, context)
        self._contextCombo.setCurrentIndex(0)
        self._contextCombo.currentIndexChanged.connect(
            self._onMaterialContextChanged)
        toolbar.addWidget(self._contextCombo)
        toolbar.addStretch()
        root.addLayout(toolbar)

        # splitter: material list | input editor
        self._splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        splitter = self._splitter

        self._matList = QtWidgets.QListWidget()
        self._matList.currentItemChanged.connect(self._onMaterialClicked)
        splitter.addWidget(self._matList)

        right = QtWidgets.QWidget()
        rightLay = QtWidgets.QVBoxLayout(right)
        rightLay.setContentsMargins(4, 0, 0, 0)

        # header: back button + shader label (with context menu)
        headerRow = QtWidgets.QHBoxLayout()
        self._backBtn = QtWidgets.QPushButton(" Back")
        self._backBtn.setIcon(
            self.style().standardIcon(QtWidgets.QStyle.SP_ArrowLeft))
        self._backBtn.setFixedHeight(24)
        self._backBtn.clicked.connect(self._navigateBack)
        self._backBtn.hide()
        headerRow.addWidget(self._backBtn)
        self._headerLabel = QtWidgets.QLabel("Select a material")
        self._headerLabel.setWordWrap(True)
        self._headerLabel.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self._headerLabel.customContextMenuRequested.connect(
            self._showHeaderContextMenu)
        headerRow.addWidget(self._headerLabel, 1)
        rightLay.addLayout(headerRow)

        sep = QtWidgets.QFrame()
        sep.setFrameShape(QtWidgets.QFrame.NoFrame)
        sep.setFixedHeight(1)
        sep.setStyleSheet(_SEPARATOR_STYLE)
        rightLay.addWidget(sep)

        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        self._newFormWidget()
        rightLay.addWidget(self._scroll)

        splitter.addWidget(right)
        splitter.setSizes([220, 500])
        splitter.setStretchFactor(1, 1)
        root.addWidget(splitter)

        statusRow = QtWidgets.QHBoxLayout()
        statusRow.setContentsMargins(0, 0, 0, 0)
        self._followCB = QtWidgets.QCheckBox("Follow Selection")
        self._followCB.setChecked(True)
        statusRow.addWidget(self._followCB)
        statusRow.addStretch(1)
        root.addLayout(statusRow)

    def _newFormWidget(self):
        self._formContainer = QtWidgets.QWidget()
        self._formLayout = QtWidgets.QFormLayout(self._formContainer)
        self._formLayout.setFieldGrowthPolicy(
            QtWidgets.QFormLayout.ExpandingFieldsGrow
        )
        self._scroll.setWidget(self._formContainer)
        self._inputWidgets = []
        self._inputRows = {}
        self._inputFieldWidgets = {}
        self._inputLayouts = {}

    # ---- material list ----------------------------------------------------

    def _refreshMaterials(self):
        currentPath = (
            str(self._currentMaterial.GetPath())
            if self._currentMaterial and self._currentMaterial.GetPrim()
            else None)
        context = self._getSelectedMaterialContext()
        contextLabel = _getMaterialContextLabel(context)

        self._matList.blockSignals(True)
        self._matList.clear()
        stage = self._api.stage
        if not stage:
            self._matList.blockSignals(False)
            return
        count = 0
        for prim in stage.Traverse():
            material = UsdShade.Material(prim)
            if material and _findSurfaceShader(material, context):
                item = QtWidgets.QListWidgetItem(str(prim.GetPath()))
                item.setData(QtCore.Qt.UserRole, str(prim.GetPath()))
                self._matList.addItem(item)
                count += 1
        self._matList.blockSignals(False)

        self._fitMaterialListWidth()
        self._scheduleLayoutFit()
        if currentPath and self._selectMaterialByPath(currentPath):
            return
        if currentPath:
            self._showMaterial(None)
        self._setStatus(f"{count} {contextLabel} material(s) found")

    def _getSelectedMaterialContext(self):
        if not hasattr(self, "_contextCombo"):
            return _DEFAULT_MATERIAL_CONTEXT
        context = self._contextCombo.itemData(self._contextCombo.currentIndex())
        if context is None:
            return _DEFAULT_MATERIAL_CONTEXT
        return str(context)

    def _onMaterialContextChanged(self, _index):
        self._shaderStack.clear()
        self._refreshMaterials()

    def _computeMaterialListWidth(self):
        if self._matList.count() == 0:
            return _MIN_MATERIAL_LIST_WIDTH

        hint = (self._matList.sizeHintForColumn(0)
                + 2 * self._matList.frameWidth() + 24)
        screen = self.screen()
        maxWidth = _MAX_MATERIAL_LIST_WIDTH
        if screen:
            maxWidth = max(
                maxWidth,
                int(screen.availableGeometry().width() * 0.45))
        return max(_MIN_MATERIAL_LIST_WIDTH, min(hint, maxWidth))

    def _computePreferredEditorWidth(self):
        controlWidth = _MIN_EDITOR_CONTROL_WIDTH
        if self._inputWidgets:
            controlWidth = max(
                controlWidth,
                max(max(widget.sizeHint().width(),
                        widget.minimumSizeHint().width(),
                        widget.minimumWidth())
                    for widget in self._inputWidgets))

        horizontalSpacing = self._formLayout.horizontalSpacing()
        if horizontalSpacing < 0:
            horizontalSpacing = 6

        formMargins = self._formLayout.contentsMargins()
        scrollBarWidth = self._scroll.verticalScrollBar().sizeHint().width()
        preferredWidth = max(
            _MIN_EDITOR_PANE_WIDTH,
            self._inputLabelWidth
            + horizontalSpacing
            + controlWidth
            + formMargins.left()
            + formMargins.right()
            + scrollBarWidth
            + 12)

        if hasattr(self, "_formContainer") and self._formContainer is not None:
            preferredWidth = max(
                preferredWidth,
                self._formContainer.sizeHint().width() + scrollBarWidth + 12)

        return preferredWidth

    def _ensurePreferredWindowWidth(self, materialListWidth):
        extraWidth = _WINDOW_CHROME_WIDTH
        splitterWidth = self._splitter.width()
        if splitterWidth > 0:
            extraWidth = max(extraWidth, self.width() - splitterWidth)

        desiredWindowWidth = (
            materialListWidth
            + self._computePreferredEditorWidth()
            + self._splitter.handleWidth()
            + extraWidth)
        desiredWindowWidth = max(_MIN_WINDOW_WIDTH, desiredWindowWidth)
        self.setMinimumWidth(desiredWindowWidth)
        if self.width() < desiredWindowWidth:
            self.resize(desiredWindowWidth, self.height())

    def _fitMaterialListWidth(self):
        hint = self._computeMaterialListWidth()
        self._ensurePreferredWindowWidth(hint)
        total = self._splitter.width() or self.width()
        self._splitter.setSizes(
            [hint, max(self._computePreferredEditorWidth(), total - hint)])

    def _scheduleLayoutFit(self):
        if self._fitScheduled:
            return
        self._fitScheduled = True
        QtCore.QTimer.singleShot(0, self._applyScheduledLayoutFit)

    def _applyScheduledLayoutFit(self):
        self._fitScheduled = False
        self._fitMaterialListWidth()

    def _onMaterialClicked(self, current, _previous):
        if not current:
            return
        path = current.data(QtCore.Qt.UserRole)
        prim = self._api.stage.GetPrimAtPath(path)
        if prim:
            self._showMaterial(UsdShade.Material(prim))

    def _selectMaterialByPath(self, path):
        for i in range(self._matList.count()):
            item = self._matList.item(i)
            if item.data(QtCore.Qt.UserRole) == path:
                if self._matList.currentItem() != item:
                    self._matList.setCurrentItem(item)
                return True
        return False

    # ---- follow selection -------------------------------------------------

    def _pollSelection(self):
        if not self._followCB.isChecked():
            return
        prims = self._api.selectedPrims
        if not prims:
            return
        primPath = str(prims[0].GetPath())
        if primPath == self._lastPrimPath:
            return
        self._lastPrimPath = primPath
        try:
            bound = UsdShade.MaterialBindingAPI(prims[0]).ComputeBoundMaterial()
        except Exception:
            return
        if bound and bound[0]:
            if not self._selectMaterialByPath(str(bound[0].GetPath())):
                contextLabel = _getMaterialContextLabel(
                    self._getSelectedMaterialContext())
                self._setStatus(
                    "Bound material has no "
                    f"{contextLabel} surface shader")

    # ---- shader navigation ------------------------------------------------

    def _showMaterial(self, material):
        self._currentMaterial = material
        self._shaderStack.clear()

        if not material:
            self._headerLabel.setText("No material")
            self._backBtn.hide()
            self._newFormWidget()
            return

        context = self._getSelectedMaterialContext()
        shader = _findSurfaceShader(material, context)
        if not shader:
            self._headerLabel.setText(
                f"No {_getMaterialContextLabel(context)} surface shader found")
            self._backBtn.hide()
            self._newFormWidget()
            return

        self._pushShader(shader)

    def _pushShader(self, shader):
        self._shaderStack.append(shader)
        self._displayCurrentShader()

    def _navigateBack(self):
        if len(self._shaderStack) > 1:
            self._shaderStack.pop()
            self._displayCurrentShader()

    def _navigateToConnected(self, inp):
        sources, _ = inp.GetConnectedSources()
        if not sources:
            return
        sourcePrim = sources[0].source.GetPrim()
        if not sourcePrim.IsValid():
            return
        sourceShader = UsdShade.Shader(sourcePrim)
        if sourceShader.GetPrim().IsValid():
            self._pushShader(sourceShader)
            self._setStatus(f"Navigated to {sourcePrim.GetPath()}")

    # ---- display ----------------------------------------------------------

    def _displayCurrentShader(self, preserveScroll=False):
        shader = self._shaderStack[-1]
        scrollPosition = (
            self._captureScrollPosition() if preserveScroll else None)
        self._updateHeader()
        self._newFormWidget()

        authoredMap = {inp.GetBaseName(): inp for inp in shader.GetInputs()}
        sdrNode = _getSdrNode(shader)
        sdrPropMap = self._getSdrPropMap(sdrNode)

        allNames = self._getOrderedInputNames(shader, sdrNode)
        self._inputLabelWidth = self._computeInputLabelWidth(allNames)
        overrideCount = 0
        currentPage = None
        currentLayout = self._formLayout

        for name in allNames:
            hasOverride, widget = self._makeInputWidget(
                name, shader, authoredMap, sdrPropMap)
            if not widget:
                continue

            if hasOverride:
                overrideCount += 1
            page = self._getInputPage(name, sdrPropMap)
            if page and page != currentPage:
                currentLayout = self._addCollapsibleGroup(page)
            elif not page:
                currentLayout = self._formLayout
            currentPage = page

            row = self._makeParameterRow(name, hasOverride, shader)
            self._addInputRow(name, row, widget, currentLayout)

        self._formContainer.adjustSize()
        self._fitMaterialListWidth()
        self._scheduleLayoutFit()
        if scrollPosition is not None:
            self._restoreScrollPosition(scrollPosition)
        self._setStatus(
            f"{overrideCount} session override(s), "
            f"{len(allNames) - overrideCount} original/default(s)")

    def _captureScrollPosition(self):
        return (
            self._scroll.horizontalScrollBar().value(),
            self._scroll.verticalScrollBar().value())

    def _restoreScrollPosition(self, scrollPosition):
        def restore():
            for scrollBar, value in (
                    (self._scroll.horizontalScrollBar(), scrollPosition[0]),
                    (self._scroll.verticalScrollBar(), scrollPosition[1])):
                scrollBar.setValue(max(
                    scrollBar.minimum(),
                    min(value, scrollBar.maximum())))

        restore()
        QtCore.QTimer.singleShot(0, restore)

    def _getSdrPropMap(self, sdrNode):
        sdrPropMap = {}
        if sdrNode:
            for name in sdrNode.GetShaderInputNames():
                prop = sdrNode.GetShaderInput(name)
                if prop:
                    sdrPropMap[name] = prop
        return sdrPropMap

    def _getOrderedInputNames(self, shader, sdrNode):
        orderedNames = []
        seenNames = set()

        def addName(name):
            if name not in seenNames:
                orderedNames.append(name)
                seenNames.add(name)

        if sdrNode:
            for name in sdrNode.GetShaderInputNames():
                addName(name)

        for inp in shader.GetInputs():
            addName(inp.GetBaseName())

        return orderedNames

    def _getInputPage(self, name, sdrPropMap):
        sdrProp = sdrPropMap.get(name)
        if not sdrProp:
            return ""
        return str(sdrProp.GetPage())

    def _addCollapsibleGroup(self, page):
        button = QtWidgets.QToolButton()
        button.setText(page)
        button.setCheckable(True)
        button.setChecked(True)
        button.setArrowType(QtCore.Qt.DownArrow)
        button.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
        button.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        button.setStyleSheet(_GROUP_HEADER_STYLE)

        content = QtWidgets.QWidget()
        layout = QtWidgets.QFormLayout(content)
        layout.setFieldGrowthPolicy(QtWidgets.QFormLayout.ExpandingFieldsGrow)
        layout.setContentsMargins(_GROUP_CONTENT_LEFT_MARGIN, 2, 0, 6)

        def toggled(checked):
            content.setVisible(checked)
            button.setArrowType(
                QtCore.Qt.DownArrow if checked else QtCore.Qt.RightArrow)
            self._formContainer.adjustSize()
            self._scheduleLayoutFit()

        button.toggled.connect(toggled)
        self._formLayout.addRow(button)
        self._formLayout.addRow(content)
        return layout

    def _addInputRow(self, name, row, widget, layout):
        row.setFieldWidget(widget)
        layout.addRow(row.labelWidget, widget)
        self._inputWidgets.append(widget)
        self._inputRows[name] = row
        self._inputFieldWidgets[name] = widget
        self._inputLayouts[name] = layout

    def _makeInputWidget(self, name, shader, authoredMap=None, sdrPropMap=None):
        if authoredMap is None:
            authoredMap = {
                inp.GetBaseName(): inp for inp in shader.GetInputs()}
        if sdrPropMap is None:
            sdrPropMap = self._getSdrPropMap(_getSdrNode(shader))

        inp = authoredMap.get(name)
        sdrProp = sdrPropMap.get(name)
        hasOverride = self._hasSessionInputOpinion(shader, name)

        if inp and inp.HasConnectedSource():
            return hasOverride, self._widgetConnected(inp)

        if inp:
            typeName = str(inp.GetTypeName())
            return hasOverride, self._widgetForType(
                inp, typeName, inp.Get(), sdrProp)

        if sdrProp:
            sdfType = sdrProp.GetTypeAsSdfType().GetSdfType()
            typeName = str(sdfType)
            try:
                value = sdrProp.GetDefaultValueAsSdfType()
            except Exception:
                value = None
            return hasOverride, self._widgetForType(
                _LazyInput(shader, name, sdfType), typeName, value, sdrProp)

        return False, None

    def _refreshInputRow(self, name, shader):
        hasOverride, widget = self._makeInputWidget(name, shader)
        rowObject = self._inputRows.get(name)
        oldWidget = self._inputFieldWidgets.get(name)

        if rowObject is None:
            if widget:
                self._displayCurrentShader(preserveScroll=True)
            return

        layout = self._inputLayouts.get(name, self._formLayout)

        if not widget:
            if oldWidget in self._inputWidgets:
                self._inputWidgets.remove(oldWidget)
            layout.removeRow(rowObject.labelWidget)
            self._inputRows.pop(name, None)
            self._inputFieldWidgets.pop(name, None)
            self._inputLayouts.pop(name, None)
            rowObject.deleteLater()
            return

        row, _role = layout.getWidgetPosition(rowObject.labelWidget)
        if row < 0:
            self._displayCurrentShader(preserveScroll=True)
            return

        rowObject.setAuthored(hasOverride)
        if oldWidget:
            layout.removeWidget(oldWidget)
            oldWidget.setParent(None)
            oldWidget.deleteLater()
            if oldWidget in self._inputWidgets:
                self._inputWidgets.remove(oldWidget)

        layout.setWidget(row, QtWidgets.QFormLayout.FieldRole, widget)
        rowObject.setFieldWidget(widget)
        self._inputWidgets.append(widget)
        self._inputFieldWidgets[name] = widget
        self._inputLayouts[name] = layout
        self._formContainer.adjustSize()
        self._fitMaterialListWidth()
        self._scheduleLayoutFit()

    def _updateHeader(self):
        shader = self._shaderStack[-1]
        shaderId = _getShaderIdStr(shader)

        if len(self._shaderStack) > 1:
            self._backBtn.show()
            crumbs = " &gt; ".join(
                _getShaderIdStr(s) or s.GetPath().name
                for s in self._shaderStack)
            self._headerLabel.setText(
                f"<span style='color:#888;'>{crumbs}</span><br>"
                f"Shader: <b>{shaderId}</b>")
        else:
            self._backBtn.hide()
            self._headerLabel.setText(f"Shader: <b>{shaderId}</b>")

    # ---- labels with context menu -----------------------------------------

    def _computeInputLabelWidth(self, names):
        referenceNames = list(names) + [_LABEL_WIDTH_REFERENCE]
        normalMetrics = QtGui.QFontMetrics(self.font())
        italicFont = QtGui.QFont(self.font())
        italicFont.setItalic(True)
        italicMetrics = QtGui.QFontMetrics(italicFont)
        return max(
            max(normalMetrics.horizontalAdvance(name) for name in referenceNames),
            max(italicMetrics.horizontalAdvance(name) for name in referenceNames)
        ) + _LABEL_WIDTH_PADDING

    def _makeParameterRow(self, name, authored, shader):
        row = ParameterRow(
            name,
            authored=authored,
            labelWidth=self._inputLabelWidth,
            parent=self._formContainer)
        row.resetRequested.connect(
            lambda n=name, s=shader: self._resetInput(n, s))
        return row

    def _refreshCurrentShaderLabelStyles(self):
        if not self._shaderStack:
            return
        shader = self._shaderStack[-1]
        for name, row in self._inputRows.items():
            row.setAuthored(self._hasSessionInputOpinion(shader, name))

    def _showHeaderContextMenu(self, pos):
        if not self._shaderStack:
            return
        menu = QtWidgets.QMenu(self)
        resetAllAction = menu.addAction("Reset All to Original")
        resetAllAction.setEnabled(self._hasAnySessionInputOpinion(
            self._shaderStack[-1]))
        action = menu.exec_(self._headerLabel.mapToGlobal(pos))
        if action == resetAllAction:
            self._resetAllInputs()

    # ---- reset actions ----------------------------------------------------

    def _resetInput(self, name, shader):
        if not self._hasSessionInputOpinion(shader, name):
            return
        self._setSessionLayerEditTarget()
        prim = shader.GetPrim()
        propName = f"inputs:{name}"
        self._pendingValues.pop(name, None)
        prim.RemoveProperty(propName)
        self._api.UpdateViewport()
        self._refreshInputRow(name, shader)
        self._setStatus(f"Reset {name}")

    def _resetAllInputs(self):
        if not self._shaderStack:
            return
        shader = self._shaderStack[-1]
        prim = shader.GetPrim()
        removed = 0
        removedNames = []
        self._setSessionLayerEditTarget()
        with Sdf.ChangeBlock():
            for inp in list(shader.GetInputs()):
                if self._hasSessionInputOpinion(shader, inp.GetBaseName()):
                    prim.RemoveProperty(inp.GetFullName())
                    removedNames.append(inp.GetBaseName())
                    removed += 1
        for name in removedNames:
            self._pendingValues.pop(name, None)
        self._api.UpdateViewport()
        for name in removedNames:
            self._refreshInputRow(name, shader)
        self._setStatus(f"Reset {removed} input(s)")

    # ---- shader input editor ----------------------------------------------

    def _buildRow(self, inp):
        name = inp.GetBaseName()
        typeName = str(inp.GetTypeName())

        label = QtWidgets.QLabel(name)
        label.setToolTip(f"{inp.GetFullName()}  ({typeName})")

        if inp.HasConnectedSource():
            return label, self._widgetConnected(inp)

        value = inp.Get()
        return label, self._widgetForType(inp, typeName, value)

    # ---- type dispatch ----------------------------------------------------

    def _widgetForType(self, inp, typeName, value, sdrProp=None):
        if typeName in ("float", "half", "double"):
            return self._widgetFloat(inp, value, sdrProp)
        if typeName in ("color3f", "color3d", "color3h"):
            return self._widgetColor(inp, value)
        if typeName == "int":
            return self._widgetInt(inp, value)
        if typeName == "bool":
            return self._widgetBool(inp, value)
        if typeName in ("float3", "vector3f", "normal3f", "point3f"):
            return self._widgetVec3(inp, value)
        if typeName in ("string", "token"):
            return self._widgetString(inp, value)
        if typeName == "asset":
            return self._widgetAsset(inp, value)
        return QtWidgets.QLabel(f"{value}  ({typeName})")

    # ---- connected input --------------------------------------------------

    def _widgetConnected(self, inp):
        w = QtWidgets.QWidget()
        lay = QtWidgets.QHBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)

        sources, _ = inp.GetConnectedSources()
        canNavigate = False
        if sources:
            sourcePrim = sources[0].source.GetPrim()
            canNavigate = sourcePrim.IsValid() and sourcePrim.IsA(UsdShade.Shader)

        navBtn = QtWidgets.QPushButton()
        navBtn.setIcon(
            self.style().standardIcon(QtWidgets.QStyle.SP_ArrowRight))
        navBtn.setFixedSize(28, 22)
        navBtn.setToolTip("Show connected node parameters")
        navBtn.setEnabled(canNavigate)
        navBtn.clicked.connect(lambda _=False, i=inp: self._navigateToConnected(i))
        lay.addWidget(navBtn)

        txt = ", ".join(
            f"{s.source.GetPath().name}.{s.sourceName}" for s in sources
        ) if sources else "connected"
        lbl = QtWidgets.QLabel(txt)
        lbl.setStyleSheet("color: #6a9bd2;")
        lbl.setToolTip(
            ", ".join(f"{s.source.GetPath()}.{s.sourceName}" for s in sources)
            if sources else "")
        lay.addWidget(lbl, 1)

        disconnBtn = QtWidgets.QPushButton()
        disconnBtn.setIcon(
            self.style().standardIcon(QtWidgets.QStyle.SP_DialogDiscardButton))
        disconnBtn.setFixedSize(22, 22)
        disconnBtn.setToolTip("Disconnect")
        disconnBtn.clicked.connect(lambda _=False, i=inp: self._disconnect(i))
        lay.addWidget(disconnBtn)

        return w

    # ---- float ------------------------------------------------------------

    def _widgetFloat(self, inp, value, sdrProp=None):
        fmin, fmax = _getFloatRange(inp.GetBaseName(), sdrProp)
        control = FloatControl(
            value=float(value) if value is not None else None,
            sliderRange=(fmin, fmax),
            spinRange=(-1e6, 1e6),
            step=0.01,
            decimals=4,
            spinWidth=_FLOAT_SPINBOX_WIDTH,
            sliderSteps=_SLIDER_STEPS,
            showSlider=True)
        control.valueChanged.connect(lambda v, i=inp: self._setValue(i, v))
        return control

    # ---- color3 -----------------------------------------------------------

    def _widgetColor(self, inp, value):
        control = Color3Control(
            value=value,
            valueRange=(0.0, 1.0),
            step=0.01,
            decimals=3,
            spinWidth=_FLOAT_SPINBOX_WIDTH,
            swatchSize=(_COLOR_SWATCH_WIDTH, _COLOR_SWATCH_HEIGHT),
            dialogParent=self,
            dialogTitle=inp.GetBaseName(),
            configureBasicColors=_configureBasicColors)
        control.valueChanged.connect(
            lambda rgb, i=inp: self._setValue(i, Gf.Vec3f(*rgb)))
        return control

    # ---- int --------------------------------------------------------------

    def _widgetInt(self, inp, value):
        control = IntControl(
            value=value,
            valueRange=(-999999, 999999),
            spinWidth=_FLOAT_SPINBOX_WIDTH)
        control.valueChanged.connect(lambda v, i=inp: self._setValue(i, v))
        return control

    # ---- bool -------------------------------------------------------------

    def _widgetBool(self, inp, value):
        cb = BoolControl(value=value)
        cb.toggled.connect(lambda v, i=inp: self._setValue(i, v))
        return cb

    # ---- vec3 -------------------------------------------------------------

    def _widgetVec3(self, inp, value):
        control = Vec3Control(
            value=value,
            valueRange=(-1e6, 1e6),
            step=0.01,
            decimals=3,
            spinWidth=_FLOAT_SPINBOX_WIDTH,
            alignRight=True)
        control.valueChanged.connect(
            lambda xyz, i=inp: self._setValue(i, Gf.Vec3f(*xyz)))
        return control

    # ---- string / token ---------------------------------------------------

    def _widgetString(self, inp, value):
        le = QtWidgets.QLineEdit(str(value) if value is not None else "")
        le.editingFinished.connect(lambda i=inp, e=le: self._setValue(i, e.text()))
        return le

    # ---- asset ------------------------------------------------------------

    def _widgetAsset(self, inp, value):
        w = QtWidgets.QWidget()
        lay = QtWidgets.QHBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)

        le = QtWidgets.QLineEdit()
        if value is not None:
            le.setText(str(value.path) if hasattr(value, "path") else str(value))

        btn = QtWidgets.QPushButton("\u2026")  # ellipsis
        btn.setFixedWidth(28)

        def browse():
            path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Select File")
            if path:
                le.setText(path)
                self._setValue(inp, Sdf.AssetPath(path))

        btn.clicked.connect(browse)
        le.editingFinished.connect(
            lambda: self._setValue(inp, Sdf.AssetPath(le.text())))

        lay.addWidget(le, 1)
        lay.addWidget(btn)
        return w

    # ---- value actions ----------------------------------------------------

    def _setValue(self, inp, value):
        self._pendingValues[inp.GetBaseName()] = (inp, value)
        self._setStatus(f"{inp.GetBaseName()} = {value}")
        self._flushTimer.start()

    def _flushPendingValues(self):
        if not self._pendingValues:
            return
        try:
            self._setSessionLayerEditTarget()
            with Sdf.ChangeBlock():
                for _key, (inp, value) in self._pendingValues.items():
                    inp.Set(value)
        except Exception as e:
            self._setStatus(f"Error: {e}")
        self._pendingValues.clear()
        self._refreshCurrentShaderLabelStyles()
        self._api.UpdateViewport()

    def _disconnect(self, inp):
        try:
            self._setSessionLayerEditTarget()
            inp.DisconnectSource()
            self._api.UpdateViewport()
            self._displayCurrentShader()
            self._setStatus(f"Disconnected {inp.GetBaseName()}")
        except Exception as e:
            self._setStatus(f"Error: {e}")

    def _setSessionLayerEditTarget(self):
        stage = self._api.stage
        if not stage:
            return False
        stage.SetEditTarget(stage.GetSessionLayer())
        return True

    def _hasSessionInputOpinion(self, shader, name):
        stage = self._api.stage
        if not stage:
            return False
        primSpec = stage.GetSessionLayer().GetPrimAtPath(
            shader.GetPrim().GetPath())
        return bool(primSpec and f"inputs:{name}" in primSpec.attributes)

    def _hasAnySessionInputOpinion(self, shader):
        stage = self._api.stage
        if not stage:
            return False
        primSpec = stage.GetSessionLayer().GetPrimAtPath(
            shader.GetPrim().GetPath())
        if not primSpec:
            return False
        return any(name.startswith("inputs:") for name in primSpec.attributes)

    def _setStatus(self, msg):
        pass

    def showEvent(self, event):
        super().showEvent(event)
        self._scheduleLayoutFit()

    def closeEvent(self, event):
        self._pollTimer.stop()
        self._flushTimer.stop()
        if self._pendingValues:
            self._flushPendingValues()
        super().closeEvent(event)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _getMaterialContextLabel(context):
    return "default" if context == "" else str(context)


def _findSurfaceShader(material, context):
    output = material.GetSurfaceOutput(context)
    if not output:
        return None

    try:
        attrs = UsdShade.Utils.GetValueProducingAttributes(
            output, True)
    except Exception:
        attrs = []

    for attr in attrs:
        shader = UsdShade.Shader(attr.GetPrim())
        if shader.GetPrim().IsValid():
            return shader
    return None


def _getSdrNode(shader):
    """Look up the Sdr definition for a UsdShade.Shader."""
    for sourceType in ("mtlx", ""):
        node = shader.GetShaderNodeForSourceType(sourceType)
        if node:
            return node

    shaderId = shader.GetShaderId()
    if not shaderId:
        return None
    reg = Sdr.Registry()
    node = reg.GetShaderNodeByIdentifier(shaderId)
    if not node:
        node = reg.GetShaderNodeByIdentifier(shaderId, ["mtlx"])
    if not node:
        node = reg.GetShaderNodeByIdentifier(shaderId, ["OSL"])
    return node


def _getShaderIdStr(shader):
    idAttr = shader.GetIdAttr()
    if idAttr and idAttr.Get():
        return str(idAttr.Get())
    return ""


def _configureBasicColors():
    """Populate Qt's shared basic-color swatches with a hue/value grid."""
    for row in range(_BASIC_COLOR_ROWS):
        if _BASIC_COLOR_ROWS <= 1:
            value = 255
        else:
            valueT = row / float(_BASIC_COLOR_ROWS - 1)
            value = int(round(
                255 + (_BASIC_COLOR_MIN_VALUE - 255) * valueT))
        for col in range(_BASIC_COLOR_COLS):
            hue = int(round(
                (360.0 * (_BASIC_COLOR_COLS - 1 - col)) / _BASIC_COLOR_COLS)) % 360
            index = col * _BASIC_COLOR_ROWS + row
            QtWidgets.QColorDialog.setStandardColor(
                index, QtGui.QColor.fromHsv(hue, 255, value))
