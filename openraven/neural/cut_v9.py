# cut_v9.py - v9 tiles: per frame the most detailed, the least detailed non-black and one random
# 1024x1024 tile; space frames get 2 smooth tiles. Usage: cut_v9.py OUTDIR
import glob, sys, random, numpy as np
from PIL import Image
out = sys.argv[1]; random.seed(9); n = {'d': 0, 's': 0, 'r': 0}
sets = [('sp', 'neural/capture_space/input', 2), ('sm', 'neural/capture_v9/smooth', 1), ('dt', 'neural/capture_v9/detail', 1)]
def ok(a, b): return abs(a[1] - b[1]) >= 640 or abs(a[2] - b[2]) >= 640
for tag, d, nsm in sets:
    for f in sorted(glob.glob(d + '/*.png')):
        im = Image.open(f).convert('RGB')
        g = np.asarray(im.reduce(4), dtype=np.float32).mean(2)
        det = np.abs(np.diff(g, axis=0))[:, :-1] + np.abs(np.diff(g, axis=1))[:-1, :]
        c = []
        for y in range(0, 2160 - 1024 + 1, 128):
            for x in range(0, 3840 - 1024 + 1, 128):
                m = g[y // 4:(y + 1024) // 4, x // 4:(x + 1024) // 4].mean()
                if m >= 8: c.append((det[y // 4:(y + 1024) // 4, x // 4:(x + 1024) // 4].mean(), y, x))
        if not c: continue
        c.sort(reverse=True); picks = [('d', c[0])]
        for s in reversed(c):
            if len([p for p in picks if p[0] == 's']) >= nsm: break
            if all(ok(s, p[1]) for p in picks): picks.append(('s', s))
        r = random.choice(c)
        if all(ok(r, p[1]) for p in picks): picks.append(('r', r))
        b = f.split('/')[-1][:-4]
        for j, (k, (_, y, x)) in enumerate(picks):
            im.crop((x, y, x + 1024, y + 1024)).save(f'{out}/v9{tag}_{b}_{k}{j}.png'); n[k] += 1
print(n)
