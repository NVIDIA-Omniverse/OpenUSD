"""Viewport dome light manipulator for RenderLab."""

from pxr import Gf, Sdf, UsdGeom, UsdLux
from pxr.Usdviewq.qt import QtCore, QtWidgets


_API_ATTR = "_renderLabDomeLightManipulator"
_DEGREES_PER_PIXEL = 0.25
_preferredDomeLightPathGetter = None


def SetPreferredDomeLightPathGetter(getter):
    global _preferredDomeLightPathGetter
    _preferredDomeLightPathGetter = getter


def Install(usdviewApi, replace=False):
    """Install the RenderLab viewport dome light manipulator."""

    existing = getattr(usdviewApi, _API_ATTR, None)
    if existing is not None:
        if not replace:
            return existing
        uninstall = getattr(existing, "Uninstall", None)
        if uninstall is not None:
            uninstall()

    manipulator = _DomeLightViewportManipulator(usdviewApi)
    QtWidgets.QApplication.instance().installEventFilter(manipulator)
    setattr(usdviewApi, _API_ATTR, manipulator)
    return manipulator


class _DomeLightViewportManipulator(QtCore.QObject):

    def __init__(self, usdviewApi):
        super().__init__()
        self._api = usdviewApi
        self._dragging = False
        self._lastX = 0.0
        self._lastY = 0.0
        self._domePath = None
        self._axis = "Y"

    def Uninstall(self):
        app = QtWidgets.QApplication.instance()
        if app is not None:
            app.removeEventFilter(self)
        self._dragging = False

    def eventFilter(self, widget, event):
        try:
            if not self._isStageViewEvent(widget):
                return False

            eventType = event.type()
            if eventType == QtCore.QEvent.MouseButtonPress:
                return self._mousePress(event)
            if eventType == QtCore.QEvent.MouseMove and self._dragging:
                return self._mouseMove(event)
            if eventType == QtCore.QEvent.MouseButtonRelease and self._dragging:
                return self._mouseRelease(event)
        except RuntimeError:
            self._dragging = False
            return False
        except Exception as err:
            self._dragging = False
            self._status("RenderLab Dome Light Drag error: {}".format(err))
            return True

        return False

    def _mousePress(self, event):
        if event.button() != QtCore.Qt.LeftButton:
            return False
        modifiers = event.modifiers()
        alt = modifiers & (QtCore.Qt.AltModifier | QtCore.Qt.MetaModifier)
        shift = modifiers & QtCore.Qt.ShiftModifier
        if not (alt and shift):
            return False

        prim = self._findDomeLightPrim()
        if prim is None:
            return False

        stage = self._api.stage
        self._domePath = prim.GetPath()
        self._axis = _rotationAxis(stage, prim)
        self._lastX, self._lastY = self._eventPos(event)
        self._dragging = True
        self._status(
            "RenderLab: Alt+Shift+LMB rotating {} around {}-axis".format(
                self._domePath, self._axis))
        event.accept()
        return True

    def _mouseMove(self, event):
        if not (event.buttons() & QtCore.Qt.LeftButton):
            self._dragging = False
            return False

        x, y = self._eventPos(event)
        dx = x - self._lastX
        dy = y - self._lastY
        if dx == 0 and dy == 0:
            return True

        if dx:
            self._rotateDome(dx * _DEGREES_PER_PIXEL)

        self._lastX = x
        self._lastY = y
        event.accept()
        return True

    def _mouseRelease(self, event):
        if event.button() == QtCore.Qt.LeftButton:
            self._dragging = False
            event.accept()
            return True
        return False

    def _rotateDome(self, degrees):
        stage = self._api.stage
        if stage is None or self._domePath is None:
            return

        prim = stage.GetPrimAtPath(self._domePath)
        if not _isDomeLightPrim(prim):
            return

        stage.SetEditTarget(stage.GetSessionLayer())
        op, component = _rotationOp(prim, self._axis, create=True)
        if op is None:
            return

        value = op.Get()
        if component is None:
            op.Set(float(value or 0.0) + degrees)
        else:
            if value is None:
                value = Gf.Vec3f(0.0)
            value = Gf.Vec3f(value)
            value[component] += degrees
            op.Set(value)

        self._api.UpdateViewport()

    def _findDomeLightPrim(self):
        stage = self._api.stage
        if stage is None:
            return None

        prim = _preferredDomeLightPrim(stage)
        if prim is not None:
            return prim

        prim = getattr(self._api, "prim", None)
        if _isDomeLightPrim(prim):
            return prim

        for prim in getattr(self._api, "selectedPrims", []):
            if _isDomeLightPrim(prim):
                return prim

        for prim in stage.Traverse():
            if _isDomeLightPrim(prim):
                return prim
        return None

    def _isStageViewEvent(self, widget):
        stageView = _stageView(self._api)
        if stageView is None:
            return False
        return widget is stageView or (
            isinstance(widget, QtWidgets.QWidget) and
            stageView.isAncestorOf(widget))

    def _eventPos(self, event):
        stageView = _stageView(self._api)
        pixelRatio = 1.0
        if stageView is not None:
            pixelRatio = stageView.devicePixelRatioF()
        return event.x() * pixelRatio, event.y() * pixelRatio

    def _status(self, message):
        try:
            self._api.PrintStatus(message)
        except Exception:
            print(message)


def _stageView(usdviewApi):
    stageView = getattr(usdviewApi, "stageView", None)
    if stageView:
        return stageView

    stageView = getattr(usdviewApi, "_stageView", None)
    if stageView:
        return stageView

    appController = getattr(usdviewApi, "_UsdviewApi__appController", None)
    if appController:
        return getattr(appController, "_stageView", None)

    return None


def _preferredDomeLightPrim(stage):
    if _preferredDomeLightPathGetter is None:
        return None

    path = _preferredDomeLightPathGetter()
    if not path:
        return None

    prim = stage.GetPrimAtPath(Sdf.Path(str(path)))
    return prim if _isDomeLightPrim(prim) else None


def _isDomeLightPrim(prim):
    if not prim or not prim.IsValid():
        return False

    for schemaName in ("DomeLight", "DomeLight_1"):
        schema = getattr(UsdLux, schemaName, None)
        if schema is not None and prim.IsA(schema):
            return True
    return False


def _rotationAxis(stage, prim):
    poleAxisAttr = prim.GetAttribute("inputs:poleAxis")
    if poleAxisAttr:
        poleAxis = poleAxisAttr.Get()
        if str(poleAxis) == "Y":
            return "Y"
        if str(poleAxis) == "Z":
            return "Z"

    return "Z" if UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.z else "Y"


def _rotationOp(prim, axis, create=False):
    xformable = UsdGeom.Xformable(prim)
    getAxisOp = getattr(xformable, "GetRotate{}Op".format(axis))
    op = getAxisOp()
    if op and op.IsDefined():
        return op, None

    rotateXYZ = xformable.GetRotateXYZOp()
    if rotateXYZ and rotateXYZ.IsDefined():
        return rotateXYZ, 1 if axis == "Y" else 2

    if not create:
        return None, None

    addAxisOp = getattr(xformable, "AddRotate{}Op".format(axis))
    return addAxisOp(), None
