import os
import tempfile
import unittest

from blackout import state


class StateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        os.environ["BLACKOUT_STATE"] = os.path.join(self.tmp, "state.json")

    def test_no_state_initially(self):
        self.assertIsNone(state.read_state())
        self.assertFalse(state.is_active())

    def test_write_then_read(self):
        state.write_state(1234, 9000)
        s = state.read_state()
        self.assertEqual(s["overlay_pid"], 1234)
        self.assertEqual(s["saved_brightness"], 9000)
        self.assertTrue(state.is_active())

    def test_clear(self):
        state.write_state(1, 2)
        state.clear_state()
        self.assertFalse(state.is_active())

    def test_corrupt_file_reads_as_none(self):
        with open(os.environ["BLACKOUT_STATE"], "w") as f:
            f.write("not json{")
        self.assertIsNone(state.read_state())


if __name__ == "__main__":
    unittest.main()
