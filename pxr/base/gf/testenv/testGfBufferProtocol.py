#!/pxrpythonsubst
#
# Copyright 2026 Pixar
#
# Licensed under the terms set forth in the LICENSE.txt file available at
# https://openusd.org/license.
#
"""Exercise Gf's buffer formats without an optional NumPy dependency."""

import gc
import struct
import unittest

from pxr import Gf


class TestGfBufferProtocol(unittest.TestCase):
    def _CheckBuffer(self, value, fmt, shape, elements):
        itemsize = struct.calcsize("@" + fmt)
        with memoryview(value) as view:
            self.assertEqual(view.format, fmt)
            self.assertEqual(view.itemsize, itemsize)
            self.assertEqual(view.shape, shape)
            self.assertEqual(view.ndim, len(shape))
            expectedStrides = ((itemsize,) if len(shape) == 1 else
                               (shape[1] * itemsize, itemsize))
            self.assertEqual(view.strides, expectedStrides)
            self.assertEqual(view.nbytes, len(elements) * itemsize)
            self.assertTrue(view.c_contiguous)
            self.assertFalse(view.readonly)
            self.assertEqual(struct.unpack("@" + fmt * len(elements), view.tobytes()),
                             tuple(elements))

            # Byte access also works for half floats, whose 'e' format cannot
            # be indexed directly by memoryview on all supported Pythons.
            with view.cast("B") as raw:
                raw[:itemsize] = struct.pack("@" + fmt, 7)
            self.assertEqual(value[0] if len(shape) == 1 else value[0][0], 7)
            del value
            gc.collect()
            self.assertEqual(struct.unpack("@" + fmt * len(elements), view.tobytes()),
                             (7, *elements[1:]))

    def test_Vectors(self):
        for dimension in (2, 3, 4):
            for suffix, fmt in (("i", "i"), ("h", "e"), ("f", "f"), ("d", "d")):
                name = f"Vec{dimension}{suffix}"
                with self.subTest(type=name):
                    values = list(range(1, dimension + 1))
                    self._CheckBuffer(getattr(Gf, name)(*values), fmt,
                                      (dimension,), values)

    def test_Matrices(self):
        for dimension in (2, 3, 4):
            for suffix, fmt in (("f", "f"), ("d", "d")):
                name = f"Matrix{dimension}{suffix}"
                with self.subTest(type=name):
                    values = list(range(1, dimension * dimension + 1))
                    self._CheckBuffer(getattr(Gf, name)(*values), fmt,
                                      (dimension, dimension), values)


if __name__ == "__main__":
    unittest.main()
