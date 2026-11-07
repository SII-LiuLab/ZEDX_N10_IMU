#!/usr/bin/env python3
"""Compare selected corresponding color patches, using paper features only to locate them.

No photometric fitting: reported differences include exposure, illumination,
camera processing and registration errors. A phone JPEG is not a color standard.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def alignment(reference, camera, polygon):
    mask = np.zeros(reference.shape[:2], np.uint8)
    cv2.fillConvexPoly(mask, np.int32(polygon), 255)
    sift = cv2.SIFT_create(nfeatures=8000, contrastThreshold=.02)
    kr, dr = sift.detectAndCompute(cv2.cvtColor(reference, cv2.COLOR_BGR2GRAY), mask)
    kc, dc = sift.detectAndCompute(cv2.cvtColor(camera, cv2.COLOR_BGR2GRAY), None)
    if dr is None or dc is None:
        raise ValueError('No reference/camera features')
    matcher = cv2.BFMatcher()
    forward = matcher.knnMatch(dr, dc, k=2)
    reverse = matcher.knnMatch(dc, dr, k=2)
    back = {a.queryIdx: a.trainIdx for a, b in reverse if a.distance < .75*b.distance}
    matches = [a for a, b in forward if a.distance < .75*b.distance and back.get(a.trainIdx) == a.queryIdx]
    if len(matches) < 20:
        raise ValueError(f'Only {len(matches)} mutually consistent matches')
    src = np.float32([kc[m.trainIdx].pt for m in matches])
    dst = np.float32([kr[m.queryIdx].pt for m in matches])
    order = np.random.default_rng(32).permutation(len(matches))
    test, train = order[:max(5, len(order)//5)], order[max(5, len(order)//5):]
    h, inliers = cv2.findHomography(src[train], dst[train], cv2.RANSAC, 4.0)
    if h is None or inliers.sum() < 15:
        raise ValueError('No reliable planar registration')
    projected = cv2.perspectiveTransform(src[:, None], h)[:, 0]
    errors = np.linalg.norm(projected-dst, axis=1)
    stats = {'mutual_matches': len(matches), 'fit_matches': len(train),
             'fit_inliers': int(inliers.sum()), 'heldout_matches': len(test),
             'heldout_error_reference_px_p50_p90': np.percentile(errors[test], [50, 90]).tolist(),
             'heldout_within_4px_fraction': float((errors[test] <= 4).mean()),
             'homography_camera_to_reference': h.tolist()}
    if np.median(errors[test]) > 4 or (errors[test] <= 4).mean() < .6:
        raise ValueError(f'Registration check failed: {stats}')
    return h, mask, stats, src, dst, errors



def sample(image, x, y, radius):
    if x-radius < 0 or y-radius < 0 or x+radius >= image.shape[1] or y+radius >= image.shape[0]:
        raise ValueError('Sample outside image')
    return np.median(image[y-radius:y+radius+1, x-radius:x+radius+1], axis=(0, 1))[::-1]


def compare(reference_path, frames, points_path, output):
    reference = cv2.imread(str(reference_path))
    points = json.loads(points_path.read_text())
    validation = json.loads((frames/'validation.json').read_text())
    images = {'reference': reference}
    locations, report = {}, {}
    for name, entry in validation['streams'].items():
        path = frames/entry['previews'][0]['png_file']
        images[name] = cv2.imread(str(path))
        file_hash = digest(path)
        if all(name in item.get('verified_camera_samples', {}) for item in points['samples'].values()):
            if points.get('camera_image_sha256', {}).get(name) != file_hash:
                raise ValueError(f'Verified sample coordinates do not belong to this image: {name}')
            locations[name] = {'geometry': {'method': 'visually_reviewed_fixed_pattern_locations'},
                               'path': str(path), 'sha256': file_hash}
        else:
            h, _, geometry, *_ = alignment(reference, images[name], np.float32(points['paper_polygon']))
            locations[name] = {'inverse': np.linalg.inv(h), 'geometry': geometry, 'path': str(path), 'sha256': file_hash}
    output.mkdir(parents=True, exist_ok=False)
    annotated = {name: im.copy() for name, im in images.items()}
    for name, item in points['samples'].items():
        x, y, radius = item['reference_x_y_radius']
        ref = sample(reference, x, y, radius)
        entries = {'reference': {'xy': [x, y], 'radius': radius, 'rgb_median': ref.tolist(), 'rgb_center': reference[y,x,::-1].tolist()}}
        for camera, loc in locations.items():
            if 'inverse' not in loc:
                px, py, rr = item['verified_camera_samples'][camera]
            else:
                mapped = cv2.perspectiveTransform(np.float32([[[x,y],[x+radius,y],[x,y+radius]]]), loc['inverse'])[0]
                px, py = np.rint(mapped[0]).astype(int)
                rr = max(1, int(round(np.linalg.norm(mapped[1:]-mapped[0], axis=1).mean())))
            rgb = sample(images[camera], px, py, rr)
            entries[camera] = {'xy': [int(px), int(py)], 'radius': rr, 'rgb_median': rgb.tolist(),
                               'rgb_center': images[camera][py,px,::-1].tolist(), 'delta_rgb_vs_reference': (rgb-ref).tolist()}
        report[name] = entries
        for camera, entry in entries.items():
            px, py = entry['xy'];rr=entry['radius']
            cv2.rectangle(annotated[camera], (px-rr,py-rr), (px+rr,py+rr), (0,0,255), 2)
            cv2.putText(annotated[camera], name, (px+rr+5,py-5), cv2.FONT_HERSHEY_SIMPLEX,.6,(0,0,255),1)
    for name, image in annotated.items():
        cv2.imwrite(str(output/(name+'-locations.jpg')), image)
    cols = list(images)
    sheet = np.full((60+len(report)*170, len(cols)*260, 3), 245, np.uint8)
    for i, name in enumerate(cols):
        cv2.putText(sheet, name, (i*260+10,30), cv2.FONT_HERSHEY_SIMPLEX,.7,(0,0,0),1)
    rows=[]
    for j,(name,entries) in enumerate(report.items()):
        for i,camera in enumerate(cols):
            e=entries[camera];x,y=e['xy'];rr=e['radius'];pad=rr+8
            im=images[camera];crop=im[max(0,y-pad):y+pad+1,max(0,x-pad):x+pad+1]
            tile=cv2.resize(crop,(110,110),interpolation=cv2.INTER_NEAREST)
            center=55; half=max(2,int(rr*110/(2*pad+1)))
            cv2.rectangle(tile,(center-half,center-half),(center+half,center+half),(0,0,255),1)
            top,left=60+j*170,i*260
            sheet[top+22:top+132,left+5:left+115]=tile
            sheet[top+22:top+102,left+125:left+245]=np.uint8(e['rgb_median'][::-1])
            cv2.putText(sheet,name,(left+5,top+15),cv2.FONT_HERSHEY_SIMPLEX,.5,(0,0,0),1)
            label='/'.join(str(round(v)) for v in e['rgb_median'])
            cv2.putText(sheet,'RGB '+label,(left+5,top+154),cv2.FONT_HERSHEY_SIMPLEX,.55,(0,0,0),1)
            rows.append([name,camera,*e['xy'],rr,*e['rgb_median'],*e['rgb_center'],*e.get('delta_rgb_vs_reference',[0,0,0])])
    cv2.imwrite(str(output/'color-patches.jpg'),sheet)
    with (output/'color-patches.csv').open('w') as f:
        writer=csv.writer(f);writer.writerow(['sample','camera','x','y','radius','median_R','median_G','median_B','center_R','center_G','center_B','delta_R','delta_G','delta_B']);writer.writerows(rows)
    result={'reference_sha256':digest(reference_path),'points':points,'samples':report,
            'location_checks':{n:{k:v for k,v in info.items() if k!='inverse'} for n,info in locations.items()},
            'scope':'Selected corresponding patterns only; no whole-image color fitting or equality claim. Fabric location is an approximate extension of the paper plane.',
            'note':'Decoded RGB 0..255, no brightness normalization. Phone JPEG is not colorimetric ground truth. Small logo colors are sensitive to resolution, registration and sampling.',
            'lamp_reference_available':False}
    (output/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference',type=Path)
    parser.add_argument('frames',type=Path)
    parser.add_argument('points',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    compare(args.reference,args.frames,args.points,args.output)
