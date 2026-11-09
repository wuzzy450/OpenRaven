# flat_blend.py - v9 targets: where the game tile is flat (low local contrast), pull the SeedVR2 target back
# toward the game tile, so the net learns to leave gradients/space/haze alone. Detailed areas keep the teacher.
# Usage: flat_blend.py PAIRS_IN OUTDIR PAIRS_OUT
import sys, os, numpy as np
from PIL import Image
from scipy.ndimage import uniform_filter
pin, od, pout = sys.argv[1:4]; os.makedirs(od, exist_ok=True); lines = []
for l in open(pin):
    a, t = l.split()
    A = np.asarray(Image.open(a).convert('RGB'), np.float32); Tt = np.asarray(Image.open(t).convert('RGB'), np.float32)
    h, w = min(A.shape[0], Tt.shape[0]), min(A.shape[1], Tt.shape[1]); A, Tt = A[:h, :w], Tt[:h, :w]
    c = uniform_filter(np.abs(A - uniform_filter(A, (5, 5, 1))).mean(2), 21)    # local contrast of the game tile
    k = np.clip((c - 0.6) / (2.5 - 0.6), 0, 1); k = k * k * (3 - 2 * k)           # 0 flat .. 1 detailed
    wgt = (0.1 + 0.9 * k)[..., None]
    o = od + '/' + os.path.basename(t)
    Image.fromarray((A + (Tt - A) * wgt).round().clip(0, 255).astype(np.uint8)).save(o)
    lines.append(f'{a} {o}\n')
open(pout, 'w').writelines(lines); print(len(lines), 'blended pairs')
