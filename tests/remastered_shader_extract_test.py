import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "extractor", Path(__file__).resolve().parents[1] / "tools/remastered_shader_extract.py")
extractor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extractor)


class ResolveTests(unittest.TestCase):
    def test_room_local_and_identical_promoted_copy(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            room = root / "rooms/pillar/3c7f869c-3e6a-4c11-a529-28d824229b31.MTRL"
            room.parent.mkdir(parents=True)
            room.write_bytes(b"owned fixture")
            self.assertEqual(extractor.resolve(root, "9C867F3C"), room)
            promoted = root / "x/all" / room.name
            promoted.parent.mkdir(parents=True)
            promoted.write_bytes(room.read_bytes())
            self.assertEqual(extractor.resolve(root, "9c867f3c").read_bytes(), b"owned fixture")
            promoted.write_bytes(b"different version")
            with self.assertRaisesRegex(ValueError, "conflicting"):
                extractor.resolve(root, "9c867f3c")

    def test_missing_and_invalid_id(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(ValueError, "no MTRL"):
                extractor.resolve(Path(folder), "9c867f3c")
            with self.assertRaisesRegex(ValueError, "eight hexadecimal"):
                extractor.resolve(Path(folder), "../")


if __name__ == "__main__":
    unittest.main()
