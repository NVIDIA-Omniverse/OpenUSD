"""Viewport camera prim manipulator for RenderLab."""

from pxr import Gf, Sdf, Usd, UsdGeom
from pxr.Usdviewq.freeCamera import FreeCamera
from pxr.Usdviewq.qt import QtCore, QtWidgets


_API_ATTR = "_renderLabCameraManipulator"
_DEGREES_PER_PIXEL = 0.25
_preferredCameraPathGetter = None
_enabledGetter = None


def SetPreferredCameraPathGetter(getter):
    global _preferredCameraPathGetter
    _preferredCameraPathGetter = getter


def SetEnabledGetter(getter):
    global _enabledGetter
    _enabledGetter = getter


def Install(usdviewApi, replace=False):
    """Install the RenderLab viewport camera manipulator."""

    existing = getattr(usdviewApi, _API_ATTR, None)
    if existing is not None:
        if not replace:
            return existing
        uninstall = getattr(existing, "Uninstall", None)
        if uninstall is not None:
            uninstall()

    manipulator = _CameraViewportManipulator(usdviewApi)
    QtWidgets.QApplication.instance().installEventFilter(manipulator)
    setattr(usdviewApi, _API_ATTR, manipulator)
    return manipulator


class _CameraViewportManipulator(QtCore.QObject):

    def __init__(self, usdviewApi):
        super().__init__()
        self._api = usdviewApi
        self._dragging = False
        self._lastX = 0.0
        self._lastY = 0.0
        self._cameraPath = None
        self._freeCamera = None
        self._mode = "none"

    def Uninstall(self):
        app = QtWidgets.QApplication.instance()
        if app is not None:
            app.removeEventFilter(self)
        self._dragging = False
        self._freeCamera = None
        self._mode = "none"

    def eventFilter(self, widget, event):
        try:
            if not self._isStageViewEvent(widget):
                return False

            if not self._enabled():
                self._dragging = False
                return False

            eventType = event.type()
            if eventType == QtCore.QEvent.MouseButtonPress:
                return self._mousePress(event)
            if eventType == QtCore.QEvent.MouseMove and self._dragging:
                return self._mouseMove(event)
            if eventType == QtCore.QEvent.MouseButtonRelease and self._dragging:
                return self._mouseRelease(event)
            if eventType == QtCore.QEvent.Wheel:
                return self._wheel(event)
        except RuntimeError:
            self._dragging = False
            self._freeCamera = None
            return False
        except Exception as err:
            self._dragging = False
            self._freeCamera = None
            self._status("RenderLab Camera Drag error: {}".format(err))
            return True

        return False

    def _mousePress(self, event):
        modifiers = event.modifiers()
        alt = modifiers & (QtCore.Qt.AltModifier | QtCore.Qt.MetaModifier)
        shift = modifiers & QtCore.Qt.ShiftModifier
        if not alt or shift:
            return False

        mode = "none"
        if event.button() == QtCore.Qt.LeftButton:
            ctrl = modifiers & QtCore.Qt.ControlModifier
            mode = "truck" if ctrl else "tumble"
        elif event.button() == QtCore.Qt.MiddleButton:
            mode = "truck"
        elif event.button() == QtCore.Qt.RightButton:
            mode = "zoom"

        if mode == "none":
            return False

        prim = self._findCameraPrim()
        if prim is None:
            return False

        if not self._startManipulation(prim):
            return False

        self._mode = mode
        self._lastX, self._lastY = self._eventPos(event)
        self._dragging = True
        self._status(
            "RenderLab: Alt-drag editing camera {}".format(self._cameraPath))
        event.accept()
        return True

    def _mouseMove(self, event):
        if self._freeCamera is None:
            self._dragging = False
            return False

        x, y = self._eventPos(event)
        dx = x - self._lastX
        dy = y - self._lastY
        if dx == 0 and dy == 0:
            return True

        if self._mode == "tumble":
            self._freeCamera.Tumble(_DEGREES_PER_PIXEL * dx,
                                    _DEGREES_PER_PIXEL * dy)
            self._applyCamera()
        elif self._mode == "zoom":
            self._zoomFromPixels(dx + dy)
        elif self._mode == "truck":
            height = self._viewportHeight()
            pixelsToWorld = self._freeCamera.ComputePixelsToWorldFactor(height)
            self._freeCamera.Truck(-dx * pixelsToWorld, dy * pixelsToWorld)
            self._applyCamera()

        self._lastX = x
        self._lastY = y
        event.accept()
        return True

    def _mouseRelease(self, event):
        self._dragging = False
        self._freeCamera = None
        self._mode = "none"
        event.accept()
        return True

    def _wheel(self, event):
        prim = self._findCameraPrim()
        if prim is None:
            return False

        if not self._startManipulation(prim):
            return False

        delta = event.angleDelta().y()
        if delta == 0:
            self._freeCamera = None
            event.accept()
            return True

        self._zoomFromScale(1 - max(-0.5, min(0.5, delta / 1000.0)))
        self._freeCamera = None
        event.accept()
        return True

    def _zoomFromPixels(self, pixels):
        zoomDelta = -0.002 * pixels
        self._zoomFromScale(1 + zoomDelta)

    def _zoomFromScale(self, scale):
        if self._freeCamera.orthographic:
            self._freeCamera.fov *= scale
            self._applyCamera(writeFrustum=True)
        else:
            self._freeCamera.AdjustDistance(scale)
            self._applyCamera()

    def _startManipulation(self, prim):
        stage = self._api.stage
        if stage is None:
            return False

        self._cameraPath = prim.GetPath()
        gfCamera = self._sourceGfCamera(prim)
        self._freeCamera = self._freeCameraForCurrentView(gfCamera)
        gfCamera = self._freeCamera.computeGfCamera(Gf.BBox3d(), autoClip=False)
        self._writeCameraTransform(prim, gfCamera)

        try:
            self._api.dataModel.viewSettings.cameraPrim = prim
        except Exception:
            pass

        self._api.UpdateViewport()
        return True

    def _applyCamera(self, writeFrustum=False):
        stage = self._api.stage
        if stage is None or self._cameraPath is None:
            return

        prim = stage.GetPrimAtPath(self._cameraPath)
        if not _isCameraPrim(prim):
            return

        gfCamera = self._freeCamera.computeGfCamera(
            Gf.BBox3d(), autoClip=False)
        if not self._writeCameraTransform(prim, gfCamera):
            return

        if writeFrustum:
            time = self._timeCode()
            usdCamera = UsdGeom.Camera(prim)
            usdCamera.GetHorizontalApertureAttr().Set(
                gfCamera.horizontalAperture, time)
            usdCamera.GetVerticalApertureAttr().Set(
                gfCamera.verticalAperture, time)

        self._api.UpdateViewport()

    def AdoptCurrentView(self, pathText=None):
        prim = self._findCameraPrim(pathText)
        if prim is None:
            return False

        gfCamera = self._currentGfCamera()
        if gfCamera is None:
            gfCamera = UsdGeom.Camera(prim).GetCamera(self._timeCode())

        if not self._writeCameraTransform(prim, gfCamera):
            return False

        try:
            self._api.dataModel.viewSettings.cameraPrim = prim
        except Exception:
            pass

        self._api.UpdateViewport()
        return True

    def _sourceGfCamera(self, prim):
        activeCamera = getattr(self._api, "cameraPrim", None)
        if not _samePrim(activeCamera, prim):
            gfCamera = self._currentGfCamera()
            if gfCamera is not None:
                return gfCamera

        return UsdGeom.Camera(prim).GetCamera(self._timeCode())

    def _navigationFreeCamera(self, gfCamera):
        camera = Gf.Camera(gfCamera)
        camera.focusDistance = self._navigationDistance(camera)
        stage = self._api.stage
        freeCamera = FreeCamera.FromGfCamera(
            camera,
            stage is not None and
            UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.z)
        self._initializeFreeCameraScale(freeCamera)
        return freeCamera

    def _freeCameraForCurrentView(self, gfCamera):
        activeCamera = getattr(self._api, "cameraPrim", None)
        viewSettings = getattr(self._api.dataModel, "viewSettings", None)
        freeCamera = getattr(viewSettings, "freeCamera", None) if viewSettings else None
        if activeCamera is None and freeCamera is not None:
            return freeCamera.clone()

        return self._navigationFreeCamera(gfCamera)

    def _navigationDistance(self, gfCamera):
        for bbox in self._candidateBBoxes():
            frustum = gfCamera.frustum
            viewDirection = frustum.ComputeViewDirection().GetNormalized()
            toCenter = bbox.ComputeCentroid() - frustum.position
            distance = Gf.Dot(toCenter, viewDirection)
            if distance > 0.001:
                return distance

        distance = gfCamera.focusDistance
        if distance > 0.001:
            return distance
        return 1.0

    def _initializeFreeCameraScale(self, freeCamera):
        size = self._viewSize()
        if size > 0.001:
            freeCamera._selSize = size
        freeCamera._closestVisibleDist = None
        freeCamera._lastFramedDist = freeCamera.dist
        freeCamera._lastFramedClosestDist = freeCamera.dist

    def _candidateBBoxes(self):
        stageView = _stageView(self._api)
        if stageView is None:
            return []

        boxes = []
        for attrName in ("_selectionBBox", "_bbox"):
            bbox = getattr(stageView, attrName, None)
            if bbox is not None and not bbox.GetRange().IsEmpty():
                boxes.append(bbox)
        return boxes

    def _viewSize(self):
        for bbox in self._candidateBBoxes():
            size = bbox.ComputeAlignedRange().GetSize()
            maxSize = max(size[0], size[1], size[2])
            if maxSize > 0.001:
                return maxSize
        return 0.0

    def _currentGfCamera(self):
        try:
            gfCamera = self._api.currentGfCamera
        except Exception:
            return None
        return Gf.Camera(gfCamera) if gfCamera is not None else None

    def _writeCameraTransform(self, prim, gfCamera):
        stage = self._api.stage
        if stage is None:
            return False

        time = self._timeCode()
        stage.SetEditTarget(stage.GetSessionLayer())
        usdCamera = UsdGeom.Camera(prim)
        parentToWorldInverse = (
            usdCamera.ComputeParentToWorldTransform(time).GetInverse())
        localTransform = gfCamera.transform * parentToWorldInverse

        xformOp = UsdGeom.Xformable(prim).MakeMatrixXform()
        if not xformOp:
            self._status(
                "RenderLab: could not author camera transform for {}".format(
                    prim.GetPath()))
            return False

        xformOp.Set(localTransform, time)
        return True

    def _findCameraPrim(self, pathText=None):
        stage = self._api.stage
        if stage is None:
            return None

        path = pathText
        if _preferredCameraPathGetter is not None:
            path = path or _preferredCameraPathGetter()

        if path:
            prim = stage.GetPrimAtPath(Sdf.Path(str(path)))
            if _isCameraPrim(prim):
                return prim

        prim = getattr(self._api, "cameraPrim", None)
        return prim if _isCameraPrim(prim) else None

    def _enabled(self):
        if _enabledGetter is None:
            return False
        return bool(_enabledGetter())

    def _timeCode(self):
        stage = self._api.stage
        if stage is not None and stage.HasAuthoredTimeCodeRange():
            try:
                return self._api.dataModel.currentFrame
            except Exception:
                pass
        return Usd.TimeCode.Default()

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

    def _viewportHeight(self):
        stageView = _stageView(self._api)
        if stageView is not None:
            try:
                return float(max(1, stageView.GetPhysicalWindowSize()[1]))
            except Exception:
                return float(max(1, stageView.height()))
        return 1.0

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


def _isCameraPrim(prim):
    return bool(prim and prim.IsValid() and prim.IsA(UsdGeom.Camera))


def _samePrim(lhs, rhs):
    return bool(
        lhs and rhs and lhs.IsValid() and rhs.IsValid() and
        lhs.GetPath() == rhs.GetPath())
