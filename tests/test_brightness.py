import unittest
from unittest import mock

from blackout import brightness


class BrightnessTest(unittest.TestCase):
    @mock.patch("blackout.brightness.subprocess.run")
    def test_get_brightness(self, run):
        run.return_value = mock.Mock(stdout="9000\n")
        self.assertEqual(brightness.get_brightness(), 9000)

    @mock.patch("blackout.brightness.subprocess.run")
    def test_set_brightness_calls_silent_setter(self, run):
        run.return_value = mock.Mock(stdout="")
        brightness.set_brightness(0)
        called = run.call_args[0][0]
        self.assertIn("setBrightnessSilent", " ".join(called))
        self.assertIn("0", called)


if __name__ == "__main__":
    unittest.main()
