"""Evaluate saved SDK metric depth on a known flat checkerboard; no calibration writes."""
import argparse
import csv
import json
from pathlib import Path

import cv2
import numpy as np


def statistics(values):
    v = np.asarray(values, dtype=float)
    v = v[np.isfinite(v)]
    if not v.size:
        return None
    return dict(count=int(v.size), median=float(np.median(v)), mean=float(v.mean()),
                std=float(v.std()), p05=float(np.percentile(v, 5)),
                p95=float(np.percentile(v, 95)), minimum=float(v.min()), maximum=float(v.max()))


def camera_matrix(calibration):
    return np.array([[calibration['fx'], 0, calibration['cx']],
                     [0, calibration['fy'], calibration['cy']], [0, 0, 1.]], dtype=float)


def detect(image, cols, rows):
    found, corners = cv2.findChessboardCornersSB(image, (cols, rows), flags=cv2.CALIB_CB_NORMALIZE_IMAGE)
    if not found:
        raise ValueError('Full checkerboard not detected')
    grid = corners.reshape(rows, cols, 2)
    candidates = [grid, grid[::-1], grid[:, ::-1], grid[::-1, ::-1]]
    return min(candidates, key=lambda g: float(g[0, 0].sum())).reshape(-1, 2).copy()


def board_pose(corners, K, cols, rows, square_m):
    objects = np.zeros((cols*rows, 3), dtype=np.float64)
    objects[:, :2] = np.mgrid[:cols, :rows].T.reshape(-1, 2)*square_m
    ok, rvec, tvec = cv2.solvePnP(objects, corners.astype(float), K, None)
    if not ok:
        raise ValueError('Board pose estimation failed')
    rvec, tvec = cv2.solvePnPRefineLM(objects, corners.astype(float), K, None, rvec, tvec)
    rotation, _ = cv2.Rodrigues(rvec)
    center = rotation@np.array([(cols-1)*square_m/2, (rows-1)*square_m/2, 0])+tvec.ravel()
    projected, _ = cv2.projectPoints(objects, rvec, tvec, K, None)
    error = np.linalg.norm(projected.reshape(-1, 2)-corners, axis=1)
    return rotation[:, 2], float(rotation[:, 2]@tvec.ravel()), center, float(np.sqrt(np.mean(error**2)))


def board_region(corners, cols, rows, shape, inset=.2):
    """Use cell interiors between detected inner corners; exclude board edges/background."""
    objects = np.mgrid[:cols, :rows].T.reshape(-1, 2).astype(float)
    transform, _ = cv2.findHomography(objects, corners.astype(float), 0)
    inverse = np.linalg.inv(transform)
    h, w = shape
    xmin, ymin = np.maximum(np.floor(corners.min(axis=0)).astype(int), 0)
    xmax, ymax = np.minimum(np.ceil(corners.max(axis=0)).astype(int)+1, [w, h])
    yy, xx = np.mgrid[ymin:ymax, xmin:xmax]
    uv = cv2.perspectiveTransform(np.stack([xx, yy], axis=-1).astype(float).reshape(-1, 1, 2), inverse).reshape(-1, 2)
    ix, iy = np.floor(uv).astype(int).T
    fractions = uv-np.floor(uv)
    keep = ((ix >= 0) & (ix < cols-1) & (iy >= 0) & (iy < rows-1)
            & (fractions[:, 0] >= inset) & (fractions[:, 0] <= 1-inset)
            & (fractions[:, 1] >= inset) & (fractions[:, 1] <= 1-inset))
    return yy.ravel()[keep], xx.ravel()[keep], ((ix+iy) % 2)[keep]


def rays_at(x, y, K):
    return np.column_stack([(x-K[0, 2])/K[0, 0], (y-K[1, 2])/K[1, 1], np.ones(len(x))])


def fit_plane(points):
    """Robustly locate a plane, but return residuals for ALL supplied valid points."""
    points = np.asarray(points, dtype=float)
    if len(points) < 100:
        raise ValueError('Too few valid board depth pixels')
    inliers = np.ones(len(points), dtype=bool)
    for _ in range(5):
        center = points[inliers].mean(axis=0)
        shifted = points[inliers]-center
        _, vectors = np.linalg.eigh(shifted.T@shifted/len(shifted))
        normal = vectors[:, 0]
        if normal[2] < 0:
            normal = -normal
        distance = float(normal@center)
        residual = points@normal-distance
        median = np.median(residual)
        sigma = 1.4826*np.median(np.abs(residual-median))
        proposed = np.abs(residual-median) <= max(.003, 3*sigma)
        if proposed.sum() < len(points)*.5 or np.array_equal(inliers, proposed):
            break
        inliers = proposed
    return normal, distance, residual, inliers


def analyze_frame(image, right, depth, confidence, K, cols, rows, square_m):
    if depth.dtype != np.float32 or depth.shape != image.shape[:2] or confidence.shape != depth.shape:
        raise ValueError('Metric depth / image dimensions or dtype mismatch')
    corners = detect(image, cols, rows)
    other = detect(right, cols, rows).reshape(rows, cols, 2)
    candidates = [other, other[::-1], other[:, ::-1], other[::-1, ::-1]]
    def stereo_score(candidate):
        dx, dy = (corners-candidate.reshape(-1, 2)).T
        return np.median(abs(dy))+1000*np.mean(dx <= 0)
    matched = min(candidates, key=stereo_score).reshape(-1, 2).copy()
    dx, dy = (corners-matched).T
    normal_ref, distance_ref, center_ref, reprojection = board_pose(corners, K, cols, rows, square_m)
    y, x, parity = board_region(corners, cols, rows, depth.shape)
    values = depth[y, x]
    valid = np.isfinite(values) & (values > 0)
    rays = rays_at(x, y, K)
    normal, distance, residual, inliers = fit_plane(rays[valid]*values[valid, None])
    # Compare both planes at the same ray through the metric board centre.
    center_ray = center_ref/center_ref[2]
    center_depth = distance/float(normal@center_ray)
    expected_z = distance_ref/(rays[valid]@normal_ref)
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)[y, x]
    black_parity = int(np.median(gray[parity == 1]) < np.median(gray[parity == 0]))
    is_black = parity == black_parity
    residual_mm = residual*1000
    metrics = dict(board_center_depth_mm=float(center_depth*1000),
                   board_center_pnp_mm=float(center_ref[2]*1000),
                   board_center_pnp_difference_mm=float((center_depth-center_ref[2])*1000),
                   optical_axis_plane_z_mm=float(distance/normal[2]*1000),
                   perpendicular_plane_distance_mm=float(distance*1000),
                   board_valid_fraction=float(valid.mean()), board_pixels=int(len(valid)),
                   board_depth_mm=statistics(values[valid]*1000),
                   plane_rmse_mm=float(np.sqrt(np.mean(residual_mm**2))),
                   plane_abs_error_p95_mm=float(np.percentile(abs(residual_mm), 95)),
                   plane_fit_inlier_fraction=float(inliers.mean()),
                   depth_minus_pnp_plane_mm=statistics((values[valid]-expected_z)*1000),
                   pnp_reprojection_rmse_px=reprojection,
                   stereo_vertical_abs_median_px=float(np.median(abs(dy))),
                   stereo_vertical_abs_p95_px=float(np.percentile(abs(dy), 95)),
                   stereo_positive_disparity_fraction=float((dx > 0).mean()),
                   confidence_on_valid_depth=statistics(confidence[y[valid], x[valid]]),
                   depth_plane_normal=normal.tolist(), pnp_plane_normal=normal_ref.tolist())
    for label, selected in [('black', is_black), ('white', ~is_black)]:
        metrics[label+'_valid_fraction'] = float(valid[selected].mean())
        metrics[label+'_plane_residual_mm'] = statistics(residual_mm[selected[valid]])
    metrics['black_minus_white_plane_residual_mm'] = (
        metrics['black_plane_residual_mm']['median']-metrics['white_plane_residual_mm']['median'])
    return metrics, dict(corners=corners, right_corners=matched, x=x, y=y, valid=valid,
                         residual_mm=residual_mm, center_ref=center_ref)


def plot_results(output, image, depth, details, records, fixed_std, fixed_mask, cols, rows):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    annotated = image.copy()
    cv2.drawChessboardCorners(annotated, (cols, rows), details['corners'].reshape(-1, 1, 2).astype(np.float32), True)
    cv2.imwrite(str(output/'corners-left.jpg'), annotated)
    y, x, valid = details['y'], details['x'], details['valid']
    roi = np.full(depth.shape, np.nan)
    roi[y[valid], x[valid]] = depth[y[valid], x[valid]]*1000
    residual = np.full(depth.shape, np.nan)
    residual[y[valid], x[valid]] = details['residual_mm']
    fig, axes = plt.subplots(2, 2, figsize=(14, 10), constrained_layout=True)
    axes[0, 0].imshow(cv2.cvtColor(annotated, cv2.COLOR_BGR2RGB))
    axes[0, 0].set_title('Detected inner corners (native RGB)')
    limits = (int(x.min())-25, int(x.max())+25, int(y.max())+25, int(y.min())-25)
    for ax, data, title, cmap, clim in [
        (axes[0, 1], roi, 'Board depth Z (cell interiors)', 'viridis', {}),
        (axes[1, 0], residual, 'Perpendicular residual to fitted plane', 'coolwarm',
         dict(vmin=-max(1, np.percentile(abs(details['residual_mm']), 99)),
              vmax=max(1, np.percentile(abs(details['residual_mm']), 99))))]:
        im = ax.imshow(data, cmap=cmap, **clim)
        ax.set_xlim(limits[:2]); ax.set_ylim(limits[2:]); ax.set_title(title)
        fig.colorbar(im, ax=ax, label='mm')
    times = [r['elapsed_s'] for r in records]
    axes[1, 1].plot(times, [r['board_center_depth_mm'] for r in records], '.-', label='SDK depth plane centre')
    axes[1, 1].plot(times, [r['board_center_pnp_mm'] for r in records], '.-', label='Known-size image pose (shared intrinsics)')
    axes[1, 1].set(xlabel='Sensor time since first saved frame (s)', ylabel='Optical Z at board centre (mm)', title='Static board over time')
    axes[1, 1].legend()
    fig.savefig(output/'overview.png', dpi=160); plt.close(fig)
    jitter = np.full(depth.shape, np.nan)
    fy, fx = fixed_mask
    jitter[fy, fx] = fixed_std
    fig, ax = plt.subplots(figsize=(8, 6), constrained_layout=True)
    im = ax.imshow(jitter, cmap='magma', vmin=0, vmax=max(.1, np.nanpercentile(fixed_std, 99)))
    ax.set_xlim(limits[:2]); ax.set_ylim(limits[2:]); ax.set_title('Per-pixel temporal standard deviation (static ROI)')
    fig.colorbar(im, ax=ax, label='mm'); fig.savefig(output/'temporal-jitter.png', dpi=160); plt.close(fig)


def run(args):
    folder, output = args.session/'zed', args.output
    output.mkdir(parents=True, exist_ok=False)
    K = camera_matrix(json.loads((folder/'calibration.json').read_text()))
    with (folder/'frames.csv').open() as f:
        samples = [row for row in csv.DictReader(f) if row['saved'] == '1']
    first_time = int(samples[0]['timestamp_ns'])
    records, failures, corners_log, fixed_depths = [], [], [], []
    first = None
    for row in samples:
        i = row['index']
        try:
            image = cv2.imread(str(folder/(i+'-left.png')))
            right = cv2.imread(str(folder/(i+'-right.png')))
            depth = np.load(folder/(i+'-depth-m.npy'), allow_pickle=False)
            confidence = np.load(folder/(i+'-confidence.npy'), allow_pickle=False)
            metrics, details = analyze_frame(image, right, depth, confidence, K, args.cols, args.rows, args.square_mm/1000)
            metrics.update(index=int(i), elapsed_s=(int(row['timestamp_ns'])-first_time)/1e9)
            if first is None:
                first = (image, depth, details)
            reference = first[2]
            metrics['corner_motion_rms_px'] = float(np.sqrt(np.mean((details['corners']-reference['corners'])**2)))
            fixed_depths.append(depth[reference['y'], reference['x']].copy())
            records.append(metrics)
            corners_log.append(dict(index=int(i), left=details['corners'].tolist(), right=details['right_corners'].tolist()))
            print(json.dumps({k: metrics[k] for k in ('index', 'board_center_depth_mm', 'board_valid_fraction', 'plane_rmse_mm')}), flush=True)
        except (ValueError, cv2.error) as error:
            failures.append(dict(index=int(i), error=str(error)))
    if not records:
        (output/'failures.json').write_text(json.dumps(failures, indent=2))
        raise ValueError('No usable board frames; see failures.json')
    stack = np.asarray(fixed_depths, dtype=float)
    stack[~np.isfinite(stack) | (stack <= 0)] = np.nan
    counts = np.isfinite(stack).sum(axis=0)
    eligible = counts >= max(2, int(np.ceil(.9*len(records))))
    jitter = np.full(stack.shape[1], np.nan)
    jitter[eligible] = np.nanstd(stack[:, eligible], axis=0)*1000
    fields = ['board_center_depth_mm', 'board_center_pnp_mm', 'board_center_pnp_difference_mm',
              'optical_axis_plane_z_mm', 'board_valid_fraction', 'plane_rmse_mm', 'plane_abs_error_p95_mm',
              'black_minus_white_plane_residual_mm', 'black_valid_fraction', 'white_valid_fraction',
              'pnp_reprojection_rmse_px', 'stereo_vertical_abs_median_px', 'stereo_positive_disparity_fraction',
              'corner_motion_rms_px']
    summary = dict(board=dict(square_mm=args.square_mm, inner_corners=[args.cols, args.rows],
                              squares=[args.cols+1, args.rows+1]),
                   reference=dict(front_face_to_board_mm=args.front_distance_mm,
                                  optical_origin_offset_mm=None, measurement_uncertainty_mm=None,
                                  absolute_accuracy_verified=False),
                   session=str(args.session), sample_count=len(samples), analyzed_frames=len(records), failures=failures,
                   camera_matrix=K.tolist(), aggregation={name: statistics([r[name] for r in records]) for name in fields},
                   fixed_roi_temporal_std_mm=statistics(jitter), fixed_roi_temporal_eligible_fraction=float(eligible.mean()),
                   scope='Current static board/pose, unchanged SDK NEURAL settings. PnP shares camera intrinsics; front-face datum differs from optical origin. No absolute accuracy certification.',
                   records=records)
    summary['reference']['unregistered_center_difference_mm'] = summary['aggregation']['board_center_depth_mm']['median']-args.front_distance_mm
    (output/'summary.json').write_text(json.dumps(summary, indent=2, allow_nan=False))
    (output/'corners.json').write_text(json.dumps(corners_log, indent=2))
    with (output/'metrics.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=['index', 'elapsed_s']+fields)
        writer.writeheader()
        writer.writerows({name: row[name] for name in writer.fieldnames} for row in records)
    plot_results(output, *first, records, jitter, (first[2]['y'], first[2]['x']), args.cols, args.rows)
    print(json.dumps({k:v for k,v in summary.items() if k != 'records'}, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--cols', type=int, default=11)
    parser.add_argument('--rows', type=int, default=8)
    parser.add_argument('--square-mm', type=float, default=25)
    parser.add_argument('--front-distance-mm', type=float, default=360)
    args = parser.parse_args()
    if args.cols < 3 or args.rows < 3 or not np.isfinite(args.square_mm) or args.square_mm <= 0 or not np.isfinite(args.front_distance_mm) or args.front_distance_mm <= 0:
        parser.error('Invalid board dimensions or distance')
    run(args)
