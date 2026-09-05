import unittest
from unittest.mock import patch
import apple_utils


class TestGetSDKVersion(unittest.TestCase):
    def test_sdk_version_missing_raises(self):
        class Ctx:
            buildTarget = apple_utils.TARGET_IOS
            cmakeBuildArgs = ""

        with patch.object(apple_utils, 'GetSDKRoot',
                          return_value='/some/path/XRSimulator.sdk'):
            with self.assertRaises(RuntimeError):
                apple_utils.GetSDKVersion(Ctx())


if __name__ == '__main__':
