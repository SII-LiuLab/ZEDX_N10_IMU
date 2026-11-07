import sys
from pathlib import Path
import unittest

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from check_checkerboard_depth import board_pose, board_region, fit_plane, statistics


class CheckerboardDepthTests(unittest.TestCase):
    def test_tilted_plane_uses_perpendicular_residual_not_raw_z_variation(self):
        x, y = np.meshgrid(np.linspace(-.3, .3, 50), np.linspace(-.2, .2, 30))
        rays = np.column_stack([x.ravel(), y.ravel(), np.ones(x.size)])
        normal = np.array([.08, -.05, 1.])
        normal /= np.linalg.norm(normal)
        distance = .365*normal[2]
        z = distance/(rays@normal)
        self.assertGreater(np.ptp(z)*1000, 20)
        fitted, offset, residual, _ = fit_plane(rays*z[:, None])
        self.assertAlmostEqual(offset/fitted[2], .365, places=10)
        self.assertLess(np.max(abs(residual)), 1e-10)

    def test_robust_fit_still_reports_bad_depth_pixels(self):
        x, y = np.meshgrid(np.linspace(-.1, .1, 40), np.linspace(-.08, .08, 30))
        points = np.column_stack([x.ravel(), y.ravel(), np.full(x.size, .36)])
        points[::10, 2] += .02
        _, offset, residual, inliers = fit_plane(points)
        self.assertAlmostEqual(offset, .36, places=8)
        self.assertAlmostEqual(inliers.mean(), .9, places=8)
        self.assertGreater(np.percentile(abs(residual), 95)*1000, 19)

    def test_known_25mm_pattern_recovers_metric_pose(self):
        objects = np.zeros((88, 3), np.float64)
        objects[:, :2] = np.mgrid[:11, :8].T.reshape(-1, 2)*.025
        K = np.array([[719., 0, 932.], [0, 719., 603.], [0, 0, 1.]])
        rotation = np.array([.03, -.04, .02])
        translation = np.array([-.125, -.0875, .36])
        projected, _ = cv2.projectPoints(objects, rotation, translation, K, None)
        _, _, center, error = board_pose(projected.reshape(-1, 2), K, 11, 8, .025)
        matrix, _ = cv2.Rodrigues(rotation)
        expected = matrix@np.array([.125, .0875, 0])+translation
        np.testing.assert_allclose(center, expected, atol=1e-8)
        self.assertLess(error, 1e-6)

    def test_region_uses_cell_interiors_and_statistics_exclude_nonfinite(self):
        corners = np.mgrid[:11, :8].T.reshape(-1, 2)*10.+100
        y, x, parity = board_region(corners, 11, 8, (256, 256))
        self.assertTrue(np.all((x > 100) & (x < 200) & (y > 100) & (y < 170)))
        self.assertEqual(set(parity), {0, 1})
        result = statistics([1, 2, 3, np.nan, np.inf])
        self.assertEqual(result['count'], 3)
        self.assertEqual(result['median'], 2)


if __name__ == '__main__':
    unittest.main()
