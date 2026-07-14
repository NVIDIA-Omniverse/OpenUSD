#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#

from contextlib import nullcontext
import unittest
from unittest import mock

from pxr.Usdviewq import qt
from pxr.Usdviewq.stageView import StageView


class TestQt(unittest.TestCase):

    @unittest.skipUnless(qt.PySideModule == 'PySide6',
                         'QSurfaceFormat is only used with PySide6')
    @unittest.skipIf(qt.sys.platform == 'darwin',
                     'macOS only supports OpenGL through version 4.1')
    def test_usdviewFormatRequestsDesktopOpenGL45(self):
        glFormat = qt.QGLFormat()

        qt.QGLFormat.ConfigureForUsdview(glFormat)

        self.assertEqual(glFormat.renderableType(), qt.QGLFormat.OpenGL)
        self.assertEqual(
            (glFormat.majorVersion(), glFormat.minorVersion()), (4, 5))

    @unittest.skipUnless(qt.PySideModule == 'PySide6',
                         'QSurfaceFormat is only used with PySide6')
    def test_usdviewFormatPreservesMacOSOpenGLVersion(self):
        glFormat = qt.QGLFormat()
        glFormat.setVersion(4, 1)

        with mock.patch.object(qt.sys, 'platform', 'darwin'):
            qt.QGLFormat.ConfigureForUsdview(glFormat)

        self.assertEqual(glFormat.renderableType(), qt.QGLFormat.OpenGL)
        self.assertEqual(
            (glFormat.majorVersion(), glFormat.minorVersion()), (4, 1))

    @unittest.skipUnless(qt.PySideModule == 'PySide6',
                         'QSurfaceFormat is only used with PySide6')
    def test_defaultFormatIsConfiguredBeforeApplication(self):
        originalFormat = qt.QGLFormat.defaultFormat()
        try:
            qt.ConfigureDefaultGLFormat()
            glFormat = qt.QGLFormat.defaultFormat()

            self.assertEqual(glFormat.renderableType(), qt.QGLFormat.OpenGL)
            if qt.sys.platform != 'darwin':
                self.assertEqual(
                    (glFormat.majorVersion(), glFormat.minorVersion()),
                    (4, 5))
        finally:
            qt.QGLFormat.setDefaultFormat(originalFormat)

    def test_closeRendererMakesContextCurrent(self):
        class _Context:
            def isValid(self):
                return True

        class _StageView:
            _renderer = object()
            _makeTimer = staticmethod(lambda _: nullcontext())
            context = staticmethod(lambda: _Context())
            makeCurrent = mock.Mock()
            doneCurrent = mock.Mock()

        stageView = _StageView()

        StageView.closeRenderer(stageView)

        self.assertIsNone(stageView._renderer)
        stageView.makeCurrent.assert_called_once_with()
        stageView.doneCurrent.assert_called_once_with()


if __name__ == '__main__':
    unittest.main(verbosity=2)
