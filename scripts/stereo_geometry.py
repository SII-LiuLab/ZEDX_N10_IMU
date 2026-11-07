"""Check rectified stereo ordering from actual scene correspondences."""
import cv2
import numpy as np


def check_pair(left, right, depth=None, calibration=None):
    detector = cv2.SIFT_create(nfeatures=3000)
    kl, dl = detector.detectAndCompute(cv2.cvtColor(left, cv2.COLOR_BGR2GRAY), None)
    kr, dr = detector.detectAndCompute(cv2.cvtColor(right, cv2.COLOR_BGR2GRAY), None)
    if dl is None or dr is None:
        raise ValueError('Insufficient stereo texture for geometry validation')
    matcher = cv2.BFMatcher()
    reverse = {a.queryIdx: a.trainIdx for a, b in matcher.knnMatch(dr, dl, k=2) if a.distance < .7*b.distance}
    matches = [a for a, b in matcher.knnMatch(dl, dr, k=2)
               if a.distance < .7*b.distance and reverse.get(a.trainIdx) == a.queryIdx]
    if len(matches) < 30:
        raise ValueError(f'Insufficient reliable stereo correspondences: {len(matches)}')
    a = np.array([kl[m.queryIdx].pt for m in matches])
    b = np.array([kr[m.trainIdx].pt for m in matches])
    dx, dy = (a-b).T
    result = {'mutual_matches': len(matches), 'positive_disparity_fraction': float((dx > 0).mean()),
              'median_disparity_px': float(np.median(dx)), 'median_vertical_error_px': float(np.median(abs(dy)))}
    result['passed'] = result['positive_disparity_fraction'] > .9 and result['median_vertical_error_px'] < 2
    if depth is not None and calibration is not None:
        x, y = np.rint(a).astype(int).T
        inside = (x >= 0) & (x < depth.shape[1]) & (y >= 0) & (y < depth.shape[0])
        x, y = x[inside], y[inside]
        disparity = dx[inside]
        geometry_ok = (disparity > 1) & (np.abs(dy[inside]) < 1)
        measured = depth[y, x]
        accepted = geometry_ok & np.isfinite(measured) & (measured > 0)
        metric = {'matched_points_with_finite_depth': int(accepted.sum()),
                  'eligible_stereo_points': int(geometry_ok.sum()),
                  'scope': 'Internal consistency against f*baseline/disparity at matched pixels; not physical ground truth'}
        if accepted.any():
            geometric = calibration['fx']*calibration['baseline_m']/disparity[accepted]
            error = np.abs(measured[accepted]-geometric)
            metric.update(median_absolute_error_m=float(np.median(error)),
                          median_relative_error=float(np.median(error/geometric)),
                          p90_relative_error=float(np.percentile(error/geometric,90)))
        result['depth_consistency'] = metric
    return result
