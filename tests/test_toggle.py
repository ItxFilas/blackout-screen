import unittest
from unittest import mock

from blackout import toggle


class ToggleTest(unittest.TestCase):
    @mock.patch("blackout.toggle.subprocess.Popen")
    @mock.patch("blackout.toggle.brightness")
    @mock.patch("blackout.toggle.state")
    def test_activates_when_inactive(self, state, brightness, popen):
        state.is_active.return_value = False
        brightness.get_brightness.return_value = 9000
        popen.return_value = mock.Mock(pid=4321)

        toggle.toggle()

        brightness.set_brightness.assert_called_once_with(0)
        state.write_state.assert_called_once_with(4321, 9000)

    @mock.patch("blackout.toggle.os.kill")
    @mock.patch("blackout.toggle.brightness")
    @mock.patch("blackout.toggle.state")
    def test_deactivates_when_active(self, state, brightness, kill):
        state.is_active.return_value = True
        state.read_state.return_value = {
            "overlay_pid": 4321,
            "saved_brightness": 9000,
        }

        toggle.toggle()

        kill.assert_called_once()
        brightness.set_brightness.assert_called_once_with(9000)
        state.clear_state.assert_called_once()

    @mock.patch("blackout.toggle.os.kill", side_effect=ProcessLookupError)
    @mock.patch("blackout.toggle.brightness")
    @mock.patch("blackout.toggle.state")
    def test_deactivate_tolerates_dead_overlay(self, state, brightness, kill):
        state.is_active.return_value = True
        state.read_state.return_value = {
            "overlay_pid": 999999,
            "saved_brightness": 8000,
        }

        toggle.toggle()  # must not raise

        brightness.set_brightness.assert_called_once_with(8000)
        state.clear_state.assert_called_once()


if __name__ == "__main__":
    unittest.main()
