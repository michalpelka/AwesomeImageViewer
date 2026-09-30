#!/usr/bin/env python3
"""Render the PhotoViewer app icon (a loupe over a pixel grid) and pack it as cmake/AppIcon.icns.

Requires numpy + opencv-python; on macOS uses `iconutil` for the .icns.
    python3 scripts/make_icon.py
"""
import colorsys
import os
import shutil
import subprocess
import tempfile

import cv2
import numpy as np

N = 1024
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

yy, xx = np.mgrid[0:N, 0:N].astype(np.float32) + 0.5


def coverage(signed_dist):
    """Anti-aliased coverage from a signed distance in pixels (negative = inside)."""
    return np.clip(0.5 - signed_dist, 0.0, 1.0)


def over(dst_rgb, dst_a, src_rgb, src_a):
    a = src_a[..., None]
    out_a = src_a + dst_a * (1 - src_a)
    out_rgb = (src_rgb * a + dst_rgb * dst_a[..., None] * (1 - a)) / np.maximum(out_a[..., None], 1e-6)
    return out_rgb, out_a


def hue_color(u, v, dim=1.0):
    r, g, b = colorsys.hsv_to_rgb((0.62 - 0.75 * u) % 1.0, 0.72, (0.95 - 0.45 * v) * dim)
    return np.array([r, g, b], np.float32)


# --- body: macOS-style squircle (Apple grid: 824 px body in a 1024 canvas) ------------------
C, HALF, EXP = N / 2, 824 / 2, 5.0
se = (np.abs((xx - C) / HALF) ** EXP + np.abs((yy - C) / HALF) ** EXP) ** (1 / EXP)
body_a = coverage((se - 1.0) * HALF)

# Dim pixel grid background.
cells = 12
x0 = C - HALF
cell = 2 * HALF / cells
ix = np.clip(((xx - x0) / cell).astype(int), 0, cells - 1)
iy = np.clip(((yy - x0) / cell).astype(int), 0, cells - 1)
palette = np.array([[hue_color(i / (cells - 1), j / (cells - 1), 0.42) for i in range(cells)] for j in range(cells)])
body_rgb = palette[iy, ix]
fx, fy = ((xx - x0) / cell) % 1, ((yy - x0) / cell) % 1
line = ((np.minimum(fx, 1 - fx) * cell < 1.2) | (np.minimum(fy, 1 - fy) * cell < 1.2)).astype(np.float32)
body_rgb = body_rgb * (1 - 0.35 * line[..., None])
shade = np.linspace(1.05, 0.8, N, dtype=np.float32)[:, None, None]  # soft top-light
body_rgb = np.clip(body_rgb * shade, 0, 1)

rgb, alpha = np.zeros((N, N, 3), np.float32), np.zeros((N, N), np.float32)

# Drop shadow under the body.
shadow = cv2.GaussianBlur(np.roll(body_a, 14, axis=0), (0, 0), 18) * 0.45
rgb, alpha = over(rgb, alpha, np.zeros_like(rgb), shadow)
rgb, alpha = over(rgb, alpha, body_rgb, body_a)

# --- lens ----------------------------------------------------------------------------------
LX, LY, LR, RING = 452.0, 440.0, 250.0, 34.0
d = np.hypot(xx - LX, yy - LY)

# Magnified pixels: 7x7 cells across the lens, centred on one highlighted pixel.
n = 7
big = 2 * LR / n
jx = np.floor((xx - (LX - LR)) / big).astype(int)
jy = np.floor((yy - (LY - LR)) / big).astype(int)
u0, v0 = (LX - x0) / (2 * HALF), (LY - x0) / (2 * HALF)
lens_pal = np.array([[hue_color(np.clip(u0 + (i - 3) * 0.06, 0, 1), np.clip(v0 + (j - 3) * 0.06, 0, 1))
                      for i in range(n)] for j in range(n)])
lens_rgb = lens_pal[np.clip(jy, 0, n - 1), np.clip(jx, 0, n - 1)]
gx, gy = ((xx - (LX - LR)) / big) % 1, ((yy - (LY - LR)) / big) % 1
grid = ((np.minimum(gx, 1 - gx) * big < 2.0) | (np.minimum(gy, 1 - gy) * big < 2.0)).astype(np.float32)
lens_rgb = lens_rgb * (1 - 0.55 * grid[..., None])
lens_a = coverage(d - LR)
rgb, alpha = over(rgb, alpha, lens_rgb, lens_a)

# Centre-pixel marker: black then white outline, like the in-app loupe.
cx0, cy0 = LX - big / 2, LY - big / 2
box = np.maximum(np.abs(xx - LX), np.abs(yy - LY)) - big / 2
for width, colour in ((9.0, 0.0), (4.0, 1.0)):
    m = coverage(np.abs(box) - width / 2) * lens_a
    rgb, alpha = over(rgb, alpha, np.full_like(rgb, colour), m)

# Glass highlight (upper-left arc).
ang = np.arctan2(yy - LY, xx - LX)
arc = coverage(np.abs(d - (LR - 38)) - 9) * np.clip(np.cos(ang + 2.36) * 2.0 - 0.9, 0, 1)
rgb, alpha = over(rgb, alpha, np.ones_like(rgb), arc * 0.35)

# --- handle (drawn before the ring so the ring caps it) ----------------------------------------
dirx, diry = np.cos(np.pi / 4), np.sin(np.pi / 4)
hx0, hy0 = LX + dirx * (LR + RING * 0.3), LY + diry * (LR + RING * 0.3)
hx1, hy1 = LX + dirx * (LR + 205), LY + diry * (LR + 205)
t = np.clip(((xx - hx0) * (hx1 - hx0) + (yy - hy0) * (hy1 - hy0)) / ((hx1 - hx0) ** 2 + (hy1 - hy0) ** 2), 0, 1)
hd = np.hypot(xx - (hx0 + t * (hx1 - hx0)), yy - (hy0 + t * (hy1 - hy0)))
handle_a = coverage(hd - 44)
side = ((xx - hx0) * -diry + (yy - hy0) * dirx) / 44  # -1..1 across the handle
handle_rgb = np.stack([0.16 + 0.10 * (1 - side)] * 3, -1).astype(np.float32)
handle_rgb[..., 2] += 0.02
rgb, alpha = over(rgb, alpha, np.clip(handle_rgb, 0, 1), handle_a)

# --- metal ring ------------------------------------------------------------------------------
ring_a = coverage(np.abs(d - (LR + RING / 2)) - RING / 2)
g = np.clip(0.5 + 0.5 * (-(xx - LX) - (yy - LY)) / (LR * 1.4), 0, 1)  # lit from top-left
ring_rgb = np.stack([0.55 + 0.40 * g, 0.57 + 0.40 * g, 0.62 + 0.38 * g], -1).astype(np.float32)
edge = coverage(np.abs(d - (LR + RING / 2)) - (RING / 2 - 3))  # thin darker rims
ring_rgb = ring_rgb * (0.75 + 0.25 * edge[..., None])
rgb, alpha = over(rgb, alpha, np.clip(ring_rgb, 0, 1), ring_a)

# --- write -----------------------------------------------------------------------------------
img = np.dstack([rgb[..., ::-1], alpha]) * 255  # to BGRA
img = np.clip(img + 0.5, 0, 255).astype(np.uint8)
png = os.path.join(ROOT, "cmake", "AppIcon.png")
cv2.imwrite(png, img)
print("wrote", png)

if shutil.which("iconutil"):
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.mkdir(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                cv2.imwrite(os.path.join(iconset, name), cv2.resize(img, (px, px), interpolation=cv2.INTER_AREA))
        icns = os.path.join(ROOT, "cmake", "AppIcon.icns")
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", icns], check=True)
        print("wrote", icns)
