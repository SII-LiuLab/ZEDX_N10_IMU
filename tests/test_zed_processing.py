import sys
from pathlib import Path
import unittest
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from zed_raw_processing import linear_bgr, render


def mosaic(r, g, b, size=48):
    raw = np.empty((size, size), dtype=np.uint16)
    raw[0::2, 0::2] = raw[1::2, 1::2] = g+42
    raw[0::2, 1::2] = r+42
    raw[1::2, 0::2] = b+42
    return (raw << 6) | (raw >> 4)


class ZedColorTests(unittest.TestCase):
    def test_grbg_packing_and_channel_order(self):
        raw = mosaic(300, 200, 100)
        np.testing.assert_allclose(linear_bgr(raw)[3:-3, 3:-3], np.broadcast_to([100, 200, 300], (42, 42, 3)))
        raw[5, 5] ^= 1
        with self.assertRaises(ValueError):
            linear_bgr(raw)

    def test_reference_matrix_preserves_neutral(self):
        import json
        p = json.loads((Path(__file__).resolve().parents[1]/'scripts/zedx_color_profile.json').read_text())
        np.testing.assert_allclose(np.asarray(p['color_matrix_rgb_rows']).sum(1), [1]*3, atol=1e-5)

    def test_clipped_white_lamp_does_not_turn_blue(self):
        import json
        p = json.loads((Path(__file__).resolve().parents[1]/'scripts/zedx_color_profile.json').read_text())
        saturated = mosaic(981, 981, 981)
        image = render(saturated, p, p['white_level_raw10'])
        self.assertTrue((image == 255).all())
        # A lower-valued colored input is unchanged by the display limit.
        raw = mosaic(20, 30, 40)
        np.testing.assert_array_equal(render(raw, p, p['white_level_raw10']),
                                      render(raw, p, p['white_level_raw10'], clip_display_highlights=False))


if __name__ == '__main__':
    unittest.main()
