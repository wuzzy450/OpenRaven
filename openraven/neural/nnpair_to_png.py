# Converts the F9 capture (pm/g7/halo/nnpair/*_off.ppm / *_on.ppm) to PNG and deletes the PPMs.
#   python nnpair_to_png.py <nnpair folder> <output folder>
# Output: input/fNNNNNN.png (network input, no HUD) and v8_output/fNNNNNN_v8.png (what v8 made of it).
# Black frames (scene cuts) are dropped.
import sys, os, glob
import numpy as np
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
for d in ("input", "v8_output"):
    os.makedirs(os.path.join(out, d), exist_ok=True)
offs = sorted(glob.glob(os.path.join(src, "*_off.ppm")))
kept = 0
for i, off in enumerate(offs):
    n = os.path.basename(off)[:6]
    on = off.replace("_off", "_on")
    a = Image.open(off).convert("RGB")
    if np.asarray(a).mean() >= 8:
        a.save(os.path.join(out, "input", "f%s.png" % n))
        if os.path.exists(on):
            Image.open(on).convert("RGB").save(os.path.join(out, "v8_output", "f%s_v8.png" % n))
        kept += 1
    os.remove(off)
    if os.path.exists(on):
        os.remove(on)
    print("\r%d/%d converted" % (i + 1, len(offs)), end="", flush=True)
print("\n%d frames kept (black ones dropped) in %s" % (kept, out))
