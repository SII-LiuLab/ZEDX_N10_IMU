"""N10 BA10/T_R16 processing: explicit GRBG, linear WB, shared exposure, sRGB."""
import json
from pathlib import Path
import cv2
import numpy as np

def raw_path(root, device):
    root = Path(root)
    current = root / 'raw' / ('left_5frames.raw' if device == 4 else 'right_5frames.raw')
    return current if current.exists() else root / f'video{device}-first.raw'

def load_frames(filename):
    data = np.fromfile(filename, dtype='<u2')
    if not data.size or data.size % (1920 * 1200):
        raise ValueError(f'Invalid capture size: {filename}')
    frames = data.reshape(-1, 1200, 1920)
    return frames

def linear_bgr(raw16, black_level=42):
    # N10 capture uses expanded RAW10: low bits repeat the high RAW10 bits.
    raw10 = raw16 >> 6
    if not np.array_equal(raw16, (raw10 << 6) | (raw10 >> 4)):
        raise ValueError('Unexpected RAW packing; this decoder requires N10 BA10/T_R16')
    # Do not use COLOR_BayerGR2BGR: that short alias reverses R/B for GRBG.
    # Subtract before interpolation and retain fractional RAW10 precision.
    linear16 = np.rint(np.maximum(raw10.astype(np.float32)-float(black_level), 0)*64).astype(np.uint16)
    return cv2.cvtColor(linear16, cv2.COLOR_BayerGRBG2BGR_EA).astype(np.float32)/64

def load_profile(path):
    profile = json.loads(Path(path).read_text())
    if profile.get('bayer_pattern') != 'GRBG':
        raise ValueError('This sensor/format is GRBG; other Bayer patterns are not supported')
    black = float(profile['black_level_raw10'])
    if not np.isfinite(black) or not 0 <= black < 1023:
        raise ValueError('Black level must be in [0,1023) RAW10 counts')
    gains = np.asarray(profile['rgb_gains'], dtype=float)
    if gains.shape != (3,) or not np.all(np.isfinite(gains)) or np.any(gains <= 0) or np.any(gains > 8):
        raise ValueError('RGB white-balance gains must be positive and at most 8')
    matrix = np.asarray(profile.get('color_matrix_rgb_rows', np.eye(3)), dtype=float)
    if matrix.shape != (3, 3) or not np.isfinite(matrix).all():
        raise ValueError('Color matrix must be finite 3x3 RGB output rows')
    if 'white_level_raw10' in profile and not 0 < float(profile['white_level_raw10']) < 8192:
        raise ValueError('Invalid fixed white level')
    return profile

def balance(linear, profile):
    return linear * np.array(profile['rgb_gains'][::-1], dtype=np.float32)

def shared_white_level(last_frames, profile):
    if 'white_level_raw10' in profile:
        return float(profile['white_level_raw10'])
    samples = [balance(linear_bgr(frame, profile['black_level_raw10']), profile)[::4, ::4].max(axis=2)
               for frame in last_frames]
    # Legacy profiles: ignore clipped light sources instead of letting them
    # suppress the whole room. A measured fixed white level is preferred.
    usable = [s[(s > 0) & (s < .9*(1023-profile['black_level_raw10']))] for s in samples]
    if not any(s.size for s in usable):
        raise ValueError('No unsaturated image signal')
    value = float(np.percentile(np.concatenate(usable), 98))
    if value <= 0:
        raise ValueError('No usable image signal')
    return value

def render(raw16, profile, white_level, apply_color_matrix=True, clip_display_highlights=True):
    sensor = linear_bgr(raw16, profile['black_level_raw10'])
    linear = balance(sensor, profile)
    if apply_color_matrix:
        matrix = np.asarray(profile.get('color_matrix_rgb_rows', np.eye(3)), dtype=np.float32)
        linear = (linear[..., ::-1] @ matrix.T)[..., ::-1]
    if not np.isfinite(white_level) or white_level <= 0:
        raise ValueError('White level must be positive and finite')
    linear = np.maximum(linear / white_level, 0)
    if clip_display_highlights:
        # Clipped RAW channels have lost their original ratios. Normalizing
        # their WB/CCM-amplified peak caused blue lamps; display clipping does
        # not pretend to reconstruct the lost highlight color or detail.
        base = linear / np.maximum(linear.max(axis=2, keepdims=True), 1)
        weight = np.clip((sensor.max(axis=2, keepdims=True)/(1023-profile['black_level_raw10'])-.90)/.09, 0, 1)
        weight = weight*weight*(3-2*weight)
        linear = base*(1-weight) + np.minimum(linear, 1)*weight
    else:
        # Keep the existing stereo-matching input unchanged by display fixes.
        linear /= np.maximum(linear.max(axis=2, keepdims=True), 1)
    srgb = np.where(linear <= 0.0031308, 12.92 * linear,
                    1.055 * np.power(linear, 1 / 2.4) - 0.055)
    return np.rint(srgb * 255).astype(np.uint8)
