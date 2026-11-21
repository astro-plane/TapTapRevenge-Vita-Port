#!/usr/bin/env python3
"""256-colour (8-bit indexed) PNGs for the Vita's home screen and LiveArea,
which must be 8-bit indexed (else installing fails with 0x8010113D). The palette
is chosen in a perceptual colour space (k-means in CIELAB) and the dithering
is gentle serpentine error diffusion, which leaves much less grain on glows and
dark gradients than a plain median-cut palette.

    python3 tools/livearea_png.py art.png sce_sys/livearea/contents/bg.png

Resize the art to the target size first (icon0 128x128, bg 840x500, startup
280x158). Needs numpy, Pillow and scikit-learn."""
import sys
import numpy as np
from PIL import Image
from sklearn.cluster import KMeans

def srgb_to_lab(rgb):                         # rgb 0..1, shape (..., 3)
    c = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    M = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
    xyz = c @ M.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 216 / 24389, np.cbrt(xyz), (24389 / 27 * xyz + 16) / 116)
    L = 116 * f[..., 1] - 16; a = 500 * (f[..., 0] - f[..., 1]); b = 200 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], -1)

def quantize(img, colors=256, strength=0.8, seed=1):
    rgb = np.asarray(img.convert('RGB')).astype(np.float64) / 255
    h, w, _ = rgb.shape
    lab = srgb_to_lab(rgb).reshape(-1, 3)
    rng = np.random.default_rng(seed)
    sample = lab[rng.choice(len(lab), min(len(lab), 120000), replace=False)]
    km = KMeans(n_clusters=colors, n_init=1, max_iter=60, random_state=seed).fit(sample)
    # each palette colour: the mean RGB of the pixels nearest it (no Lab->RGB round trip)
    lab_c = km.cluster_centers_
    idx = km.predict(lab)
    pal = np.zeros((colors, 3))
    for k in range(colors):
        m = idx == k
        pal[k] = rgb.reshape(-1, 3)[m].mean(0) if m.any() else 0
    pal_lab = srgb_to_lab(pal)
    # nearest palette entry for any colour, by a 64-level-per-channel lookup table
    g = (np.arange(64) + 0.5) / 64
    grid = np.stack(np.meshgrid(g, g, g, indexing='ij'), -1).reshape(-1, 3)
    glab = srgb_to_lab(grid)
    lut = np.empty(len(grid), np.int32)
    for s in range(0, len(grid), 16384):
        d = ((glab[s:s + 16384, None, :] - pal_lab[None]) ** 2).sum(-1)
        lut[s:s + 16384] = d.argmin(1)
    # serpentine Floyd-Steinberg, error scaled by `strength` (less grain than full strength)
    buf = rgb.copy(); out = np.zeros((h, w), np.uint8)
    for y in range(h):
        xs = range(w) if y % 2 == 0 else range(w - 1, -1, -1)
        dx = 1 if y % 2 == 0 else -1
        row = buf[y]; nxt = buf[y + 1] if y + 1 < h else None
        for x in xs:
            c = np.clip(row[x], 0, 1)
            q = (c * 63.999).astype(int)
            k = lut[(q[0] * 64 + q[1]) * 64 + q[2]]
            out[y, x] = k
            e = (c - pal[k]) * strength
            if 0 <= x + dx < w: row[x + dx] += e * (7 / 16)
            if nxt is not None:
                if 0 <= x - dx < w: nxt[x - dx] += e * (3 / 16)
                nxt[x] += e * (5 / 16)
                if 0 <= x + dx < w: nxt[x + dx] += e * (1 / 16)
    res = Image.fromarray(out, 'P')
    p = (np.clip(pal, 0, 1) * 255 + 0.5).astype(np.uint8).reshape(-1).tolist()
    res.putpalette(p + [0] * (768 - len(p)))
    return res

if __name__ == '__main__':
    quantize(Image.open(sys.argv[1]), strength=float(sys.argv[3]) if len(sys.argv) > 3 else 0.8).save(sys.argv[2], optimize=True)
