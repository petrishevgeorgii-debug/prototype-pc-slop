#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A quick software picture of Pure3D meshes (no game code, no GPU): checks that geometry decodes.

  python tools/formats/render_preview.py out.png file.p3d.rz [more files] [--view top|aerial]
"""
import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pure3d  # noqa: E402


def triangles(files, world_only=True):
    """All triangles of the files' meshes. world_only: without the props modelled around their
    own origin (they are placed by instances, which this preview does not read yet)."""
    tris = []
    for f in files:
        for _, prims in pure3d.meshes(pure3d.load(f)):
            for p in prims:
                pos = pure3d.positions(p)
                if not pos or p.kind != 0:
                    continue
                v = np.asarray(pos, dtype=np.float32)
                if world_only and np.linalg.norm(v.mean(0)) < 60 and np.ptp(v, axis=0).max() < 60:
                    continue
                idx = np.asarray(p.indices[:len(p.indices) // 3 * 3], dtype=np.int64).reshape(-1, 3)
                idx = idx[(idx < len(v)).all(axis=1)]
                tris.append(v[idx])
    return np.concatenate(tris) if tris else np.zeros((0, 3, 3), np.float32)


def render(tris, out, view, size=(1600, 1000)):
    w, h = size
    # Framed on most of the geometry: props placed at their own origin stay out of the way.
    pts = tris.reshape(-1, 3)
    lo, hi = np.percentile(pts, 2, axis=0), np.percentile(pts, 98, axis=0)
    centre = (lo + hi) / 2
    extent = float(max(hi[0] - lo[0], hi[2] - lo[2]))
    if view == 'top':
        eye = centre + np.array([0, extent * 2, 0.001])
    else:
        eye = centre + np.array([-0.45, 0.38, -0.55]) * extent
    fwd = centre - eye
    fwd /= np.linalg.norm(fwd)
    right = np.cross(fwd, [0, 1, 0])
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    rel = tris - eye
    x, y, z = rel @ right, rel @ up, rel @ fwd
    keep = (z > 1).all(axis=1)
    x, y, z, t = x[keep], y[keep], z[keep], tris[keep]
    if view == 'top':
        scale = min(w, h) / extent * 0.95
        sx, sy = w / 2 + x * scale, h / 2 - y * scale
    else:
        f = w / (2 * math.tan(math.radians(30)))
        sx, sy = w / 2 + x / z * f, h / 2 - y / z * f
    n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    sun = np.array([0.4, 0.8, 0.3])
    sun /= np.linalg.norm(sun)
    light = 0.35 + 0.65 * np.abs(n @ sun)
    height = (t[:, :, 1].mean(1) - t[:, :, 1].min()) / (np.ptp(t[:, :, 1]) + 1e-6)
    order = np.argsort(-z.mean(1))
    img = Image.new('RGB', (w, h), (24, 26, 30))
    draw = ImageDraw.Draw(img)
    for i in order:
        c = light[i] * (0.55 + 0.45 * height[i])
        col = (int(170 * c), int(160 * c), int(150 * c))
        draw.polygon([(sx[i, 0], sy[i, 0]), (sx[i, 1], sy[i, 1]), (sx[i, 2], sy[i, 2])], fill=col)
    img.save(out)
    return len(order)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out')
    p.add_argument('files', nargs='+')
    p.add_argument('--view', choices=('top', 'aerial'), default='aerial')
    a = p.parse_args()
    tris = triangles(a.files)
    print(f'{render(tris, a.out, a.view)} of {len(tris)} triangles drawn to {a.out}')


if __name__ == '__main__':
    main()
