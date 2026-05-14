"""Shared parameter widgets for RenderLab editors."""

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets


def valuesEqual(left, right):
    if right is None:
        return False
    if isinstance(left, (str, bytes)) or isinstance(right, (str, bytes)):
        leftSeq = None
        rightSeq = None
    else:
        try:
            leftSeq = tuple(left)
            rightSeq = tuple(right)
        except TypeError:
            leftSeq = None
            rightSeq = None

    if leftSeq is not None and rightSeq is not None:
        if len(leftSeq) != len(rightSeq):
            return False
        return all(valuesEqual(l, r) for l, r in zip(leftSeq, rightSeq))

    try:
        return abs(float(left) - float(right)) < 1e-6
    except (TypeError, ValueError):
        return left == right


class NoWheelSlider(QtWidgets.QSlider):

    def wheelEvent(self, event):
        event.ignore()


class NoWheelDoubleSpinBox(QtWidgets.QDoubleSpinBox):

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setKeyboardTracking(False)

    def wheelEvent(self, event):
        event.ignore()


class NoWheelSpinBox(QtWidgets.QSpinBox):

    def wheelEvent(self, event):
        event.ignore()


class FloatControl(QtWidgets.QWidget):
    """Float control with optional slider and spinbox."""

    valueChanged = QtCore.Signal(float)

    def __init__(
            self,
            value=None,
            sliderRange=(0.0, 1.0),
            spinRange=(-1e6, 1e6),
            step=0.01,
            decimals=4,
            spinWidth=None,
            sliderSteps=1000,
            showSlider=True,
            alignRight=False,
            parent=None):
        super().__init__(parent)
        self._sliderMin = float(sliderRange[0])
        self._sliderMax = float(sliderRange[1])
        self._sliderSteps = int(sliderSteps)
        self._updating = False

        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.slider = NoWheelSlider(QtCore.Qt.Horizontal)
        self.slider.setRange(0, self._sliderSteps)
        self.slider.setVisible(showSlider)
        layout.addWidget(self.slider, 1)
        if alignRight and not showSlider:
            layout.addStretch(1)

        self.spinBox = NoWheelDoubleSpinBox()
        self.spinBox.setRange(float(spinRange[0]), float(spinRange[1]))
        self.spinBox.setSingleStep(step)
        self.spinBox.setDecimals(decimals)
        if spinWidth is not None:
            self.spinBox.setFixedWidth(spinWidth)
        layout.addWidget(self.spinBox)

        self.slider.valueChanged.connect(self._onSliderChanged)
        self.spinBox.valueChanged.connect(self._onSpinChanged)

        if value is not None:
            self.setValue(float(value))

    def value(self):
        return self.spinBox.value()

    def setValue(self, value, emit=False):
        self._updating = True
        self.spinBox.setValue(float(value))
        self.slider.setValue(self._floatToSlider(float(value)))
        self._updating = False
        if emit:
            self.valueChanged.emit(float(value))

    def setSliderVisible(self, visible):
        self.slider.setVisible(bool(visible))

    def isSliderVisible(self):
        return not self.slider.isHidden()

    def _onSliderChanged(self, pos):
        if self._updating:
            return
        value = self._sliderToFloat(pos)
        self._updating = True
        self.spinBox.setValue(value)
        self._updating = False
        self.valueChanged.emit(value)

    def _onSpinChanged(self, value):
        if self._updating:
            return
        self._updating = True
        self.slider.setValue(self._floatToSlider(value))
        self._updating = False
        self.valueChanged.emit(float(value))

    def _floatToSlider(self, value):
        if self._sliderMax == self._sliderMin:
            return 0
        normalized = (
            (float(value) - self._sliderMin) /
            (self._sliderMax - self._sliderMin))
        return int(max(0, min(
            self._sliderSteps,
            normalized * self._sliderSteps)))

    def _sliderToFloat(self, pos):
        t = pos / float(self._sliderSteps)
        return self._sliderMin + t * (self._sliderMax - self._sliderMin)


class IntControl(QtWidgets.QWidget):

    valueChanged = QtCore.Signal(int)

    def __init__(
            self,
            value=None,
            valueRange=(-999999, 999999),
            spinWidth=None,
            parent=None):
        super().__init__(parent)
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addStretch(1)

        self.spinBox = NoWheelSpinBox()
        self.spinBox.setRange(int(valueRange[0]), int(valueRange[1]))
        if spinWidth is not None:
            self.spinBox.setFixedWidth(spinWidth)
        if value is not None:
            self.spinBox.setValue(int(value))
        self.spinBox.valueChanged.connect(
            lambda nextValue: self.valueChanged.emit(int(nextValue)))
        layout.addWidget(self.spinBox)

    def value(self):
        return self.spinBox.value()

    def setValue(self, value):
        self.spinBox.setValue(int(value))


class BoolControl(QtWidgets.QCheckBox):

    def __init__(self, value=None, parent=None):
        super().__init__(parent)
        if value is not None:
            self.setChecked(bool(value))

    def value(self):
        return self.isChecked()

    def setValue(self, value):
        self.setChecked(bool(value))


class ParameterRow(QtCore.QObject):
    """Shared label/reset UI for parameter rows.

    The row only owns UI state. Editors remain responsible for authored-state
    queries and for performing the actual reset.
    """

    resetRequested = QtCore.Signal()

    _defaultLabelStyle = "color: #888; font-style: italic;"
    _authoredLabelStyle = ""

    def __init__(
            self,
            name,
            fieldWidget=None,
            authored=False,
            labelWidth=None,
            resetEnabled=True,
            parent=None):
        super().__init__(parent)
        self._authored = False
        self._resetEnabled = bool(resetEnabled)
        self.labelWidget = QtWidgets.QLabel(name)
        self.fieldWidget = fieldWidget
        if labelWidth is not None:
            self.labelWidget.setFixedWidth(labelWidth)

        self.labelWidget.setContextMenuPolicy(QtCore.Qt.CustomContextMenu)
        self.labelWidget.customContextMenuRequested.connect(
            self._showContextMenu)
        self.setAuthored(authored)

    def setFieldWidget(self, widget):
        self.fieldWidget = widget

    def setAuthored(self, authored):
        self._authored = bool(authored)
        self.labelWidget.setStyleSheet(
            self._authoredLabelStyle
            if self._authored else self._defaultLabelStyle)

    def isAuthored(self):
        return self._authored

    def setResetEnabled(self, enabled):
        self._resetEnabled = bool(enabled)

    def _showContextMenu(self, pos):
        menu = QtWidgets.QMenu(self.labelWidget)
        resetAction = menu.addAction("Reset to Original")
        resetAction.setEnabled(self._resetEnabled and self._authored)
        action = menu.exec_(self.labelWidget.mapToGlobal(pos))
        if action == resetAction and resetAction.isEnabled():
            self.resetRequested.emit()


class Vec2Control(QtWidgets.QWidget):

    valueChanged = QtCore.Signal(object)

    def __init__(
            self,
            value=None,
            valueRange=(-1e6, 1e6),
            step=0.01,
            decimals=4,
            spinWidth=None,
            alignRight=False,
            parent=None):
        super().__init__(parent)
        self._spins = []
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        if alignRight:
            layout.addStretch(1)
        self._buildSpins(layout, 2, value, valueRange, step, decimals, spinWidth)

    def value(self):
        return tuple(spin.value() for spin in self._spins)

    def setValue(self, value):
        for spin, nextValue in zip(self._spins, value):
            spin.setValue(float(nextValue))

    def _buildSpins(
            self, layout, count, value, valueRange, step, decimals, spinWidth):
        for index in range(count):
            spin = NoWheelDoubleSpinBox()
            spin.setRange(float(valueRange[0]), float(valueRange[1]))
            spin.setSingleStep(step)
            spin.setDecimals(decimals)
            if spinWidth is not None:
                spin.setFixedWidth(spinWidth)
            if value is not None:
                spin.setValue(float(value[index]))
            spin.valueChanged.connect(lambda _=None: self.valueChanged.emit(
                self.value()))
            self._spins.append(spin)
            layout.addWidget(spin)


class Vec3Control(Vec2Control):

    def __init__(
            self,
            value=None,
            valueRange=(-1e6, 1e6),
            step=0.01,
            decimals=3,
            spinWidth=None,
            alignRight=False,
            parent=None):
        QtWidgets.QWidget.__init__(self, parent)
        self._spins = []
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        if alignRight:
            layout.addStretch(1)
        self._buildSpins(layout, 3, value, valueRange, step, decimals, spinWidth)


class Color3Control(QtWidgets.QWidget):

    valueChanged = QtCore.Signal(object)

    def __init__(
            self,
            value=None,
            valueRange=(0.0, 1.0),
            step=0.01,
            decimals=3,
            spinWidth=None,
            swatchSize=(36, 22),
            dialogParent=None,
            dialogTitle="Color",
            configureBasicColors=None,
            parent=None):
        super().__init__(parent)
        self._dialogParent = dialogParent or self
        self._dialogTitle = dialogTitle
        self._configureBasicColors = configureBasicColors
        self._state = [
            float(value[index]) if value is not None else 0.0
            for index in range(3)]
        self._spins = []

        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self._swatch = QtWidgets.QPushButton()
        self._swatch.setFixedSize(int(swatchSize[0]), int(swatchSize[1]))
        self._swatch.setCursor(QtCore.Qt.PointingHandCursor)
        self._swatch.clicked.connect(self._pickColor)
        layout.addWidget(self._swatch)
        layout.addStretch(1)

        for index in range(3):
            spin = NoWheelDoubleSpinBox()
            spin.setRange(float(valueRange[0]), float(valueRange[1]))
            spin.setSingleStep(step)
            spin.setDecimals(decimals)
            if spinWidth is not None:
                spin.setFixedWidth(spinWidth)
            spin.valueChanged.connect(
                lambda _=None, channel=index: self._spinChanged(channel))
            self._spins.append(spin)
            layout.addWidget(spin)

        self._refresh()

    def value(self):
        return tuple(self._state)

    def setValue(self, value):
        self._state = [float(value[index]) for index in range(3)]
        self._refresh()

    def _spinChanged(self, channel):
        self._state[channel] = self._spins[channel].value()
        self._refresh()
        self.valueChanged.emit(self.value())

    def _pickColor(self):
        if self._configureBasicColors:
            self._configureBasicColors()
        initial = QtGui.QColor.fromRgbF(
            self._clamp01(self._state[0]),
            self._clamp01(self._state[1]),
            self._clamp01(self._state[2]))
        color = QtWidgets.QColorDialog.getColor(
            initial,
            self._dialogParent,
            self._dialogTitle,
            QtWidgets.QColorDialog.DontUseNativeDialog)
        if color.isValid():
            self._state = [color.redF(), color.greenF(), color.blueF()]
            self._refresh()
            self.valueChanged.emit(self.value())

    def _refresh(self):
        red = int(round(self._clamp01(self._state[0]) * 255))
        green = int(round(self._clamp01(self._state[1]) * 255))
        blue = int(round(self._clamp01(self._state[2]) * 255))
        self._swatch.setStyleSheet(
            "background-color: rgb({},{},{});"
            " border: 1px solid #888; border-radius: 2px;".format(
                red, green, blue))
        for spin, value in zip(self._spins, self._state):
            spin.blockSignals(True)
            spin.setValue(value)
            spin.blockSignals(False)

    def _clamp01(self, value):
        return max(0.0, min(1.0, float(value)))
