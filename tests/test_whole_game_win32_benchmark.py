from pathlib import Path
import struct
import tempfile
import unittest

from training.whole_game_win32_benchmark import active_slots, fixture_entities


class Win32BenchmarkFormatTest(unittest.TestCase):
    def test_late_fixture_requires_substantial_entity_count(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.bin"
            path.write_bytes(b"PWGI1\0\0\0" + struct.pack("<III", 128, 96, 124))
            self.assertEqual(fixture_entities(path), 124)
            path.write_bytes(b"PWGI1\0\0\0" + struct.pack("<III", 128, 96, 12))
            with self.assertRaisesRegex(ValueError, "at least 100"):
                fixture_entities(path)

    def test_compiled_slot_count_is_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "output.bin"
            payload = (b"PWGO1\0\0\0" + struct.pack("<I", 1) +
                       struct.pack("<I", 10) + b"slot_count" +
                       struct.pack("<I", 1) + struct.pack("<f", 4.0))
            path.write_bytes(payload)
            self.assertEqual(active_slots(path), 4)
            path.write_bytes(payload + b"extra")
            with self.assertRaisesRegex(ValueError, "trailing"):
                active_slots(path)


if __name__ == "__main__":
    unittest.main()
