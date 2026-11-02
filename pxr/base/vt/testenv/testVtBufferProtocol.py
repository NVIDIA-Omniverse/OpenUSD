#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Buffer contracts using only the standard library."""
import array
import gc
import struct
import unittest
import weakref
from pxr import Gf, Vt


class TestBufferProtocol(unittest.TestCase):
    def test_snapshot_and_lifetime(self):
        value = Vt.IntArray([1, 2, 3])
        observation = weakref.ref(value)
        view = memoryview(value)
        self.assertTrue(view.readonly)
        self.assertEqual((view.format, view.shape, view.strides), ("i", (3,), (4,)))
        self.assertEqual(view.tobytes(), struct.pack("=3i", 1, 2, 3))
        value[0] = 99
        self.assertEqual(view.tolist(), [1, 2, 3])
        self.assertEqual(list(value), [99, 2, 3])
        with self.assertRaises(TypeError):
            view[0] = 4
        del value
        gc.collect()
        self.assertIsNotNone(observation())
        self.assertEqual(view.tolist(), [1, 2, 3])
        view.release()
        gc.collect()
        self.assertIsNone(observation())

    def test_element_shapes(self):
        cases = [
            (Vt.FloatArray([1, 2]), "f", (2,)),
            (Vt.HalfArray([1, 2]), "e", (2,)),
            (Vt.Vec3fArray([Gf.Vec3f(1, 2, 3)]), "f", (1, 3)),
            (Vt.Matrix2dArray([Gf.Matrix2d(1)]), "d", (1, 2, 2)),
            (Vt.QuatfArray([Gf.Quatf(1, Gf.Vec3f(2, 3, 4))]), "f", (1, 4)),
            (Vt.Range1dArray([Gf.Range1d(1, 2)]), "d", (1, 2)),
            (Vt.Range3fArray([Gf.Range3f(Gf.Vec3f(1), Gf.Vec3f(2))]), "f", (1, 2, 3)),
            (Vt.Rect2iArray([Gf.Rect2i(Gf.Vec2i(1), Gf.Vec2i(2))]), "i", (1, 2, 2)),
            (Vt.DualQuatdArray([Gf.DualQuatd(1)]), "d", (1, 2, 4)),
        ]
        for value, fmt, shape in cases:
            with self.subTest(type=type(value)):
                with memoryview(value) as view:
                    self.assertEqual(view.format, fmt)
                    self.assertEqual(view.shape, shape)
                    self.assertTrue(view.c_contiguous)
                    self.assertTrue(view.readonly)
                    stride = view.itemsize
                    strides = []
                    for dimension in reversed(shape):
                        strides.insert(0, stride)
                        stride *= dimension
                    self.assertEqual(view.strides, tuple(strides))
                    self.assertEqual(view.nbytes, stride)

    def test_strided_consumers_copy(self):
        source = array.array("i", range(10))
        for slice_ in (slice(None), slice(None, None, 2), slice(None, None, -1)):
            with self.subTest(slice=slice_):
                view = memoryview(source)[slice_]
                converted = Vt.DoubleArray.FromBuffer(view)
                self.assertEqual(list(converted), list(source)[slice_])
                saved = list(converted)
                source[0] = 99
                self.assertEqual(list(converted), saved)
                source[0] = 0
                view.release()
        self.assertIs(Vt.DoubleArray.FromBuffer, Vt.DoubleArray.FromNumpy)

    def test_multidimensional_consumers(self):
        source = array.array("d", range(12))
        view = memoryview(source).cast("B").cast("d", shape=[2, 2, 3])
        expected = [Gf.Vec3d(*range(i, i + 3)) for i in range(0, 12, 3)]
        self.assertEqual(list(Vt.Vec3dArray.FromBuffer(view)), expected)
        self.assertEqual(list(Vt.Vec3dArray.FromBuffer(view[::-1])),
                         expected[2:] + expected[:2])
        self.assertEqual(list(Vt.DoubleArray.FromBuffer(view)), list(source))

    def test_empty_and_errors(self):
        with memoryview(Vt.FloatArray()) as view:
            self.assertEqual(view.shape, (0,))
            self.assertEqual(view.nbytes, 0)
            self.assertEqual(len(Vt.DoubleArray.FromBuffer(view)), 0)
        for bad in (object(), array.array("d", [1, 2])):
            with self.subTest(value=type(bad)):
                with self.assertRaises(ValueError):
                    Vt.Vec3dArray.FromBuffer(bad)
        with self.assertRaises(TypeError):
            memoryview(Vt.StringArray(["not a numeric buffer"]))
        self.assertEqual(list(Vt.IntArray.FromBuffer(array.array("i", [4, 5]))), [4, 5])


if __name__ == "__main__":
    unittest.main()
