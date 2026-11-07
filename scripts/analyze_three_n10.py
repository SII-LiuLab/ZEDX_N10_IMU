#!/usr/bin/env python3
"""Generate a labelled contact sheet and corresponding reference-color samples."""
import argparse
import csv
import json
from pathlib import Path
import shutil
import cv2
import numpy as np
from compare_camera_reference import compare
from session_integrity import seal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('reference', type=Path)
    parser.add_argument('points', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--reviewed-points', action='store_true',
                        help='Use visually reviewed coordinates bound to exact image SHA256 hashes')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    images = args.output/'images'
    images.mkdir()
    entries = {}
    chosen = {}
    for name in ('shw_v6','shw_v7','zed'):
        folder = args.session/name
        with (folder/'frames.csv').open() as f:
            saved = [r for r in csv.DictReader(f) if r['saved'] == '1']
        row = saved[-1]
        i = row['index']
        source = folder/(i+'-left.png' if name == 'zed' else i+'-rgb.jpg')
        target = images/(name+source.suffix)
        shutil.copyfile(source, target)
        entries[name] = {'previews': [{'png_file': target.name}]}
        chosen[name] = {'source': str(source), 'timestamp_ns': int(row['timestamp_ns'])}
    (images/'validation.json').write_text(json.dumps({'streams': entries}))
    shutil.copyfile(args.reference, args.output/'reference.jpg')
    points = json.loads(args.points.read_text())
    if not args.reviewed_points:
        for item in points['samples'].values():
            item.pop('verified_camera_samples', None)
        points.pop('camera_image_sha256', None)
        points['note'] = 'Automatically located from paper-plane features; small logo patches require visual review.'
    (args.output/'points.json').write_text(json.dumps(points, indent=2))
    compare(args.reference, images, args.output/'points.json', args.output/'colors')
    depth = np.load(args.session/'zed'/(i+'-depth-m.npy'), allow_pickle=False)
    valid = np.isfinite(depth) & (depth > 0)
    scaled = np.zeros(depth.shape, np.uint8)
    scaled[valid] = np.clip(np.rint((depth[valid]-.2)/3.8*255),0,255).astype(np.uint8)
    colored = cv2.applyColorMap(scaled, cv2.COLORMAP_TURBO)
    colored[~valid] = 0
    cv2.imwrite(str(args.output/'zed-depth.png'), colored)
    near = cv2.applyColorMap(np.clip(np.nan_to_num((depth-.2)/.8*255, nan=0, posinf=0, neginf=0),0,255).astype(np.uint8), cv2.COLORMAP_TURBO)
    near[~valid] = 0
    cv2.imwrite(str(args.output/'zed-depth-near.png'), near)
    cv2.imwrite(str(args.output/'zed-valid.png'), valid.astype(np.uint8)*255)
    tiles = [('Reference photo', cv2.imread(str(args.reference))),
             ('SHW V6 | native ISP | RGB', cv2.imread(str(images/'shw_v6.jpg'))),
             ('SHW V7 | native ISP | RGB', cv2.imread(str(images/'shw_v7.jpg'))),
             ('ZED V8 | official SDK | rectified LEFT', cv2.imread(str(images/'zed.png'))),
             ('ZED V8 | official metric depth | 0.2-4 m', colored),
             ('ZED V8 | same depth, near display | 0.2-1 m', near)]
    width, height = 800, 650
    sheet = np.full((height*2,width*3,3),28,np.uint8)
    for n,(label,image) in enumerate(tiles):
        x,y=(n%3)*width,(n//3)*height
        cv2.putText(sheet,label,(x+12,y+30),cv2.FONT_HERSHEY_SIMPLEX,.61,(245,245,245),1,cv2.LINE_AA)
        factor=min((width-20)/image.shape[1],(height-80)/image.shape[0])
        resized=cv2.resize(image,None,fx=factor,fy=factor,interpolation=cv2.INTER_AREA)
        yy=y+45+(height-80-resized.shape[0])//2
        xx=x+(width-resized.shape[1])//2
        sheet[yy:yy+resized.shape[0],xx:xx+resized.shape[1]]=resized
        if n in (4,5):
            cv2.putText(sheet,f'Valid {valid.mean()*100:.1f}% | black = invalid, not 0 m',
                        (x+12,y+height-15),cv2.FONT_HERSHEY_SIMPLEX,.6,(245,245,245),1,cv2.LINE_AA)
    cv2.imwrite(str(args.output/'camera-reference.jpg'),sheet)
    (args.output/'selection.json').write_text(json.dumps({'frames': chosen,
        'depth_valid_fraction': float(valid.mean()),
        'scope': 'Actual images; independently sampled frames, not synchronized exposures; no color fitting or depth filling.'},indent=2)+'\n')
    seal(args.output)


if __name__ == '__main__':
    main()
