"""Cut the mountains out of a photo (GrabCut), grade them for the theme and
save the footer art with a transparent sky.

usage: cutout.py <dark|light> <src.jpg> <out dir> <preview dir>

Source of site/assets/footer-*.webp: "Matterhorn by Moonlight" by Willi
Winzig, CC BY 2.0, https://commons.wikimedia.org/wiki/File:Matterhorn_by_Moonlight_(26657356427).jpg
(the 3840 px rendition). Both themes are cut from it; the dark one is graded
to moonlight. Needs opencv-python-headless, numpy, Pillow.
"""
import sys

import cv2
import numpy as np
from PIL import Image

THEME, SRC, OUT, PREV = sys.argv[1:5]
LIGHT = THEME == "light"

im = Image.open(SRC).convert("RGB")
im = im.resize((2560, round(im.height * 2560 / im.width)), Image.LANCZOS)
full = np.asarray(im)
H, W = full.shape[:2]

# GrabCut at half size
small = cv2.resize(full, (W // 2, H // 2), interpolation=cv2.INTER_AREA)
h, w = small.shape[:2]
f = small.astype(np.float64) / 255
lum = 0.2126 * f[..., 0] + 0.7152 * f[..., 1] + 0.0722 * f[..., 2]
blue = f[..., 2] - (f[..., 0] + f[..., 1]) / 2
# the source is a moonlit photo with a dark blue night sky, for both themes
skyish = (lum < 0.30) & (blue > 0.02)

mask = np.where(skyish, cv2.GC_PR_BGD, cv2.GC_PR_FGD).astype(np.uint8)
mask[: int(h * 0.10)] = cv2.GC_BGD
mask[int(h * 0.88):] = cv2.GC_FGD
bgd, fgd = np.zeros((1, 65), np.float64), np.zeros((1, 65), np.float64)
cv2.grabCut(cv2.cvtColor(small, cv2.COLOR_RGB2BGR), mask, None, bgd, fgd, 6, cv2.GC_INIT_WITH_MASK)
fg = ((mask == cv2.GC_FGD) | (mask == cv2.GC_PR_FGD)).astype(np.uint8)

# keep the land mass that reaches the bottom; drop islands in the sky (stars, haze)
n, lab, stats, _ = cv2.connectedComponentsWithStats(fg, 8)
keep = set(lab[-1][lab[-1] > 0].tolist())
fg = np.isin(lab, list(keep)).astype(np.uint8)
# fill holes: sky cannot sit inside the mountains
inv = (1 - fg).astype(np.uint8)
n, lab, stats, _ = cv2.connectedComponentsWithStats(inv, 4)
# sky = open areas reaching the top or a side edge (above the land's foot)
edge = np.concatenate([lab[0], lab[: int(h * 0.88), 0], lab[: int(h * 0.88), -1]])
top_labels = set(edge[edge > 0].tolist())
fg = np.where(np.isin(lab, list(top_labels)), 0, 1).astype(np.uint8)
fg = cv2.morphologyEx(fg, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
# thin uprights against the sky (lift poles) go too: a pixel stays only if
# some 7-wide horizontal run of mountain covers it (an opening, done by hand)
K = 7
c = np.cumsum(np.pad(fg.astype(np.int32), ((0, 0), (1, 0))), axis=1)
full_run = (c[:, K:] - c[:, :-K]) == K               # run of K starting at x
cover = np.zeros_like(fg, dtype=bool)
for k in range(K):
    cover[:, k:k + full_run.shape[1]] |= full_run
fg = (fg.astype(bool) & cover).astype(np.uint8)
# trim sky-coloured pixels off the top of every column (patches GrabCut kept)
for x in range(w):
    col_fg = np.nonzero(fg[:, x])[0]
    if col_fg.size == 0:
        continue
    y = col_fg[0]
    while y < h and fg[y, x] and skyish[y, x]:
        fg[y, x] = 0
        y += 1

alpha = cv2.resize(fg.astype(np.float32), (W, H), interpolation=cv2.INTER_LINEAR)
alpha = cv2.GaussianBlur(alpha, (0, 0), 1.0)
alpha = np.clip((alpha - 0.5) * 1.6 + 0.5, 0, 1)

col = full.astype(np.float64) / 255
ys = np.linspace(0, 1, H)[:, None, None]
if LIGHT:
    # soft, airy: lift the shadows a touch
    col = col * 0.92 + 0.06
else:
    # moonlight: darker, cooler, sinking into the dark towards the bottom
    gray = col.mean(axis=2, keepdims=True)
    col = (gray * 0.55 + col * 0.45) * np.array([0.62, 0.72, 1.0]) * 0.62
    col *= 1.0 - 0.55 * ys ** 1.5

rows = np.nonzero(alpha.max(axis=1) > 0.5)[0]
top = int(max(0, rows.min() - 30))
rgba = np.dstack([np.clip(col, 0, 1), alpha])[top:]
main = Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA")

# The photo's right side is sky. A farther range fills it: the same massif,
# mirrored, smaller and lower, veiled in the air colour, behind the main peak.
page = (236, 238, 248) if LIGHT else (7, 5, 13)
SCALE = 0.9
far = main.transpose(Image.FLIP_LEFT_RIGHT)
far = far.resize((round(far.width * SCALE), round(far.height * SCALE)), Image.LANCZOS)
fa = np.asarray(far, np.float64) / 255
veil = 0.38 if LIGHT else 0.40
fa[..., :3] = fa[..., :3] * (1 - veil) + np.array(page) / 255 * veil
far = Image.fromarray((fa * 255 + 0.5).astype(np.uint8), "RGBA")
# put the mirrored summit near the right edge, standing on the bottom
solid = np.asarray(far)[..., 3] > 128
first_row = np.where(solid.any(axis=0), solid.argmax(axis=0), far.height)
peak_x = int(np.argmin(first_row))                 # the column of the summit
out = Image.new("RGBA", main.size, (0, 0, 0, 0))
out.alpha_composite(far, (round(main.width * 0.90) - peak_x, main.height - far.height))
out.alpha_composite(main)
print(THEME, "size", out.size)
out.save(f"{OUT}/footer-{THEME}.webp", "WEBP", quality=88, method=6)
out.resize((1280, round(out.height * 1280 / 2560)), Image.LANCZOS).save(f"{OUT}/footer-{THEME}-1280.webp", "WEBP", quality=86, method=6)

bg = Image.new("RGBA", out.size, page + (255,))
bg.alpha_composite(out)
bg.convert("RGB").resize((1280, round(out.height / 2))).save(f"{PREV}/cut-{THEME}.png")
