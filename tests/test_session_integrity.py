import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from session_integrity import seal, verify


class IntegrityTests(unittest.TestCase):
    def test_nested_camera_sessions_are_verified(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            camera = root/'zed'
            camera.mkdir()
            (camera/'frame.raw').write_bytes(b'camera frame')
            seal(camera)
            self.assertEqual(seal(root), 2)
            self.assertEqual(verify(root), 2)
            (camera/'SESSION-SHA256SUMS').write_text('corrupt nested manifest')
            with self.assertRaises(ValueError):
                verify(root)

    def test_corruption_and_unlisted_files_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'frame.raw').write_bytes(b'original frame')
            self.assertEqual(seal(root), 1)
            self.assertEqual(verify(root), 1)
            (root/'frame.raw').write_bytes(b'changed frame')
            with self.assertRaises(ValueError):
                verify(root)
            seal(root)
            (root/'extra').write_bytes(b'unlisted')
            with self.assertRaises(ValueError):
                verify(root)

    def test_manifest_cannot_escape_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root/'SESSION-SHA256SUMS').write_text('0'*64+'  ../outside\n')
            with self.assertRaises(ValueError):
                verify(root)


if __name__ == '__main__':
    unittest.main()
