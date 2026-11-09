# train3.py - neural detail pass v3: learns the LTX 2.3 remaster look (game frame -> remastered frame).
# Pairs: PAIRS file with lines "game.png remaster.png" (remaster.py output, same framing).
# Input = the GAME frame degraded like FSR 2; target = the REMASTER. Wherever the remaster changed the
# shapes (a rewritten logo, a different face), the pixels are left out of the loss, so the network only
# learns "more detail on the same thing", never "replace it".
# (Below: the v2 notes still apply.)
# train2.py - neural detail pass v2: bigger network trained to GENERATE detail (faces, textures).
#
# Teacher: game frames at 2x supersampled 4K (gfx_nn2_*). Input: same frame degraded like FSR 2
# Quality (67% render + upscale + light sharpening).
# Network (runs at half resolution after a 2x2 pixel-unshuffle): conv 12->C, (L-2) x conv C->C, conv
# C->12, all 3x3 + ReLU, pixel-shuffle residual.
# Brightness guard (also in the game shader):
#   1. the residual has its 5x5 local mean removed -> it only adds detail, never shifts brightness;
#   2. output limited to the input's 3x3 range widened by 25% (and 0..1) -> no halos, lifted blacks
#      or blown highlights, but room for new texture.
# Losses: L1, log-L1 (dark detail), gradient L1 (structure), high-frequency spectrum match (detail
# amount), and a small patch critic (adversarial) so it sharpens instead of averaging.
# Usage: nnvenv/bin/python -I train2.py OUTPREFIX STEPS DIR [DIR...]
import sys, glob, random, time
import numpy as np, torch, torch.nn as nn, torch.nn.functional as F
from PIL import Image

dev = 'cuda:1' if torch.cuda.device_count() > 1 else 'cuda:0'
import os
C, L = int(os.environ.get('NN_C', 16)), int(os.environ.get('NN_L', 4))

def guard(x, r):
    r = r - F.avg_pool2d(F.pad(r, (2, 2, 2, 2), mode='replicate'), 5, 1)
    lo = -F.max_pool2d(-x, 3, 1, 1); hi = F.max_pool2d(x, 3, 1, 1); m = 0.25 * (hi - lo)
    return torch.minimum(torch.maximum(x + r, lo - m), hi + m).clamp(0, 1)

class Net(nn.Module):
    def __init__(s):
        super().__init__()
        ch = [12] + [C] * (L - 1) + [12]
        s.convs = nn.ModuleList(nn.Conv2d(ch[i], ch[i + 1], 3, padding=1) for i in range(L))
        nn.init.zeros_(s.convs[-1].weight); nn.init.zeros_(s.convs[-1].bias)
    def residual(s, x):
        h = F.pixel_unshuffle(x, 2)
        for i, c in enumerate(s.convs):
            h = c(h)
            if i < L - 1: h = F.relu(h)
        return F.pixel_shuffle(h, 2)
    def forward(s, x): return guard(x, s.residual(x))

class Critic(nn.Module):
    def __init__(s):
        super().__init__()
        s.n = nn.Sequential(nn.Conv2d(3, 32, 4, 2, 1), nn.LeakyReLU(0.2), nn.Conv2d(32, 64, 4, 2, 1), nn.LeakyReLU(0.2),
                            nn.Conv2d(64, 128, 4, 2, 1), nn.LeakyReLU(0.2), nn.Conv2d(128, 1, 3, 1, 1))
    def forward(s, x): return s.n(x * 2 - 1)

def srgb2lin(x): return torch.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)
def lin2srgb(x): x = x.clamp(0, 1); return torch.where(x <= 0.0031308, x * 12.92, 1.055 * x.clamp(min=1e-8) ** (1 / 2.4) - 0.055)

def degrade(gt):
    B, _, H, W = gt.shape
    sc = random.choice([float(v) for v in os.environ.get('NN_SCALES', '1.5').split(',')])   # FSR render ratio
    lo = F.interpolate(gt, size=(int(H / sc), int(W / sc)), mode='area')
    up = F.interpolate(lo, size=(H, W), mode='bicubic', align_corners=False).clamp(0, 1)
    k = torch.rand(B, 1, 1, 1, device=gt.device) * 0.35
    blur = F.avg_pool2d(F.pad(up, (1, 1, 1, 1), mode='replicate'), 3, 1)
    lo3 = -F.max_pool2d(-up, 3, 1, 1); hi3 = F.max_pool2d(up, 3, 1, 1)
    return torch.minimum(torch.maximum(up + k * (up - blur), lo3), hi3)

def load(dirs):
    ims = []
    for d in dirs:
        for f in sorted(glob.glob(d + '/shot_*.png')):
            if '_gen' in f or '_ev_' in f or '_nnin' in f: continue
            a = np.asarray(Image.open(f).convert('RGB'))
            sm = a[::8, ::8].astype(np.float32)
            if a.mean() < 3 or sm.mean() > 200 or sm.std() < 8: continue   # black loading / washed-out menu frames
            ims.append(torch.from_numpy(a.copy()).permute(2, 0, 1))
    return ims

def load_pairs(path):
    P = []
    for line in open(path):
        a, b = line.split()
        tb = torch.from_numpy(np.asarray(Image.open(b).convert('RGB')).copy()).permute(2, 0, 1)
        ga = Image.open(a).convert('RGB').resize((tb.shape[2], tb.shape[1]), Image.LANCZOS)
        P.append((torch.from_numpy(np.asarray(ga).copy()).permute(2, 0, 1), tb))
    return P

def batch_pairs(P, n, p):
    A, B = [], []
    for _ in range(n):
        a, b = random.choice(P); _, H, W = a.shape
        y = random.randrange(0, H - p) & ~1; x = random.randrange(0, W - p) & ~1
        ta, tb = a[:, y:y + p, x:x + p], b[:, y:y + p, x:x + p]
        if random.random() < 0.5: ta, tb = ta.flip(2), tb.flip(2)
        A.append(ta); B.append(tb)
    a = torch.stack(A).to(dev).float() / 255; b = torch.stack(B).to(dev).float() / 255
    e = torch.exp(torch.empty(a.shape[0], 1, 1, 1, device=dev).uniform_(np.log(0.2), np.log(1.6)))
    return lin2srgb(srgb2lin(a) * e), lin2srgb(srgb2lin(b) * e)

def keep_mask(a, b):
    # 1 where the remaster kept the game's shapes (blurred difference small), 0 where it redrew them
    la, lb = a.mean(1, keepdim=True), b.mean(1, keepdim=True)
    d = (F.avg_pool2d(la, 9, 1, 4) - F.avg_pool2d(lb, 9, 1, 4)).abs()
    ga = torch.cat(grad_full(F.avg_pool2d(la, 5, 1, 2)), 1); gb = torch.cat(grad_full(F.avg_pool2d(lb, 5, 1, 2)), 1)
    s = (ga - gb).abs().sum(1, keepdim=True)
    m = ((d < 0.05) & (s < 0.06)).float()
    return -F.max_pool2d(-m, 7, 1, 3)          # erode: drop the edges of redrawn areas too

def grad_full(x): return F.pad(x[..., 1:, :] - x[..., :-1, :], (0, 0, 0, 1)), F.pad(x[..., :, 1:] - x[..., :, :-1], (0, 1, 0, 0))

def recon_m(o, g, m):
    w = m.sum().clamp(min=1)
    l = ((o - g).abs() * m).sum() / (3 * w) + 0.5 * ((torch.log(o + 0.02) - torch.log(g + 0.02)).abs() * m).sum() / (3 * w)
    for a, b in zip(grad_full(o), grad_full(g)): l = l + ((a - b).abs() * m).sum() / (3 * w)
    return l + 0.05 * (hf_spec(o) - hf_spec(g)).abs().mean()

def batch(ims, n, p):
    out = []
    for _ in range(n):
        im = random.choice(ims); _, H, W = im.shape
        sc = random.choice((1, 1, 0.75, 0.5))
        q = min(int(p / sc) & ~1, min(H, W) - 2)
        y = random.randrange(0, H - q) & ~1; x = random.randrange(0, W - q) & ~1
        t = im[:, y:y + q, x:x + q]
        if q != p: t = F.interpolate(t[None].float(), size=(p, p), mode='area')[0].round().clamp(0, 255).byte()
        if random.random() < 0.5: t = t.flip(2)
        out.append(t)
    g = torch.stack(out).to(dev).float() / 255
    e = torch.exp(torch.empty(g.shape[0], 1, 1, 1, device=dev).uniform_(np.log(0.2), np.log(1.6)))
    return lin2srgb(srgb2lin(g) * e)

def grad(x): return x[..., 1:, :] - x[..., :-1, :], x[..., :, 1:] - x[..., :, :-1]
def hf_spec(x):   # high-frequency amplitude spectrum (detail amount, independent of exact placement)
    f = torch.fft.rfft2(x.mean(1)).abs()
    h, w = f.shape[-2:]; yy = torch.fft.fftfreq(h, device=x.device).abs()[:, None]; xx = torch.fft.rfftfreq(x.shape[-1], device=x.device)[None, :]
    return torch.log1p(f * ((yy ** 2 + xx ** 2).sqrt() > 0.15))

def recon(o, g):
    l = (o - g).abs().mean() + 0.5 * (torch.log(o + 0.02) - torch.log(g + 0.02)).abs().mean()
    for a, b in zip(grad(o), grad(g)): l = l + (a - b).abs().mean()
    return l + 0.05 * (hf_spec(o) - hf_spec(g)).abs().mean()

def export_header(net, path):
    with open(path, 'w') as f:
        f.write('// generated by neural/train2.py\n#define NN_LAYERS %d\n#define NN_C %d\n' % (L, C))
        for i, c in enumerate(net.convs):
            w = c.weight.detach().cpu().float().numpy(); b = c.bias.detach().cpu().float().numpy()
            f.write('static const float nn_w%d[%d] = {%s};\n' % (i, w.size, ','.join('%.8g' % v for v in w.ravel())))
            f.write('static const float nn_b%d[%d] = {%s};\n' % (i, b.size, ','.join('%.8g' % v for v in b)))
        f.write('static const float *const nn_W[NN_LAYERS] = {%s};\nstatic const float *const nn_B[NN_LAYERS] = {%s};\n' %
                (','.join('nn_w%d' % i for i in range(L)), ','.join('nn_b%d' % i for i in range(L))))
        f.write('static const int nn_cin[NN_LAYERS] = {%s};\nstatic const int nn_cout[NN_LAYERS] = {%s};\n' %
                (','.join(str(c.in_channels) for c in net.convs), ','.join(str(c.out_channels) for c in net.convs)))

if __name__ == '__main__':
    out, steps, pairs = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    P = load_pairs(pairs); random.seed(1); random.shuffle(P)
    nval = max(2, len(P) // 10); val, tr = P[:nval], P[nval:]
    print(f'{len(tr)} train / {len(val)} val pairs on {dev}, C={C} L={L}', flush=True)
    net, crit = Net().to(dev), Critic().to(dev)
    opt = torch.optim.Adam(net.parameters(), 1e-3); optc = torch.optim.Adam(crit.parameters(), 2e-4, betas=(0.5, 0.999))
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, 1e-3, total_steps=steps)
    random.seed(2); va, vb = batch_pairs(val, 24, 256); vi = degrade(va); vm = keep_mask(va, vb)
    print(f'val: {vm.mean().item()*100:.0f}% of pixels kept (rest = redrawn by the remaster)', flush=True)
    adv_from = steps // 3; t0 = time.time()
    for it in range(steps):
        a, g = batch_pairs(tr, 16, 192)
        with torch.no_grad(): x = degrade(a); m = keep_mask(a, g)
        o = net(x); l = recon_m(o, g, m)
        if it >= adv_from: l = l + 0.01 * F.softplus(-crit(o)).mean()
        opt.zero_grad(); l.backward(); opt.step(); sched.step()
        if it >= adv_from - 500:
            dl = F.softplus(-crit(g)).mean() + F.softplus(crit(o.detach())).mean()
            optc.zero_grad(); dl.backward(); optc.step()
        if it % 1000 == 0 or it == steps - 1:
            with torch.no_grad():
                o = net(vi)
                def ps(z): return (10 * torch.log10(1 / (((z - vb) ** 2) * vm).sum() * (3 * vm.sum()))).item()
                print(f'it {it} | masked PSNR in {ps(vi):.2f} net {ps(o):.2f} | detail remaster {hf_spec(vb).mean().item():.3f} game {hf_spec(va).mean().item():.3f} in {hf_spec(vi).mean().item():.3f} net {hf_spec(o).mean().item():.3f} | brightness game {va.mean():.4f} net {o.mean():.4f} | {time.time()-t0:.0f}s', flush=True)
    torch.save(net.state_dict(), out + '.pt'); export_header(net, out + '_weights.h'); print('saved', out)
