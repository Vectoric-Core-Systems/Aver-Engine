"""NRD training (docs/rendering/NEURAA_NRD.md section 4).

Mirrors aver_denoise.hlsl's spatial estimate S (pass 4's nrdSpatialEstimate, pass 5's pyramid, the
half-rate input reconstruction). Per 8x8 tile it finds oracle corrections to the four level logits
(the parameters) that bring S closest to the converged input, then regresses a small MLP from tile
statistics onto those corrections. Only the parameters are regressed: no image loss is
back-propagated into the network, and the held-out parameter error picks the model (design doc
section 7). Image error judges the finished network only.
usage: nrd_train.py <capture root> <out weights>
"""
import glob, os, sys, time
import numpy as np
import nrd_io

Y = np.array([0.2126, 0.7152, 0.0722], np.float32)
BASE_LOGIT = np.array([-2.0, -1.0, 0.0, 0.0], np.float32)
DEPTH_SIGMA = 16.0
N_IN, N_H1, N_H2, N_OUT = 16, 16, 16, 4
DELTA_MAX = 6.0
rng = np.random.default_rng(3)


def fresh_mask(h, w, flag):
    if (flag & 1) == 0: return np.ones((h, w), bool)
    yy, xx = np.mgrid[0:h, 0:w]
    return ((xx ^ yy ^ ((flag >> 1) & 1)) & 1) == 0


def load_input(c, k):
    """dnsrLoadInput for every pixel: skipped half-rate pixels from their agreeing edge neighbours."""
    raw = c['bases'][k]; v = raw[..., :3]; z = c['z']; n = c['n']
    h, w = z.shape
    fr = fresh_mask(h, w, c['flags'][k])
    pad = lambda a: np.pad(a, [(1, 1), (1, 1)] + [(0, 0)] * (a.ndim - 2), mode='edge')
    vp, zp, np_ = pad(v), pad(z), pad(n)
    s = np.zeros_like(v); plain = np.zeros_like(v); ws = np.zeros((h, w), np.float32)
    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        vq = vp[1 + dy:h + 1 + dy, 1 + dx:w + 1 + dx]; zq = zp[1 + dy:h + 1 + dy, 1 + dx:w + 1 + dx]
        nq = np_[1 + dy:h + 1 + dy, 1 + dx:w + 1 + dx]
        wt = np.exp(-np.abs(zq - z) / np.maximum(z, 1e-3) * 32.0) * np.clip((nq * n).sum(-1), 0, 1) ** 8
        s += vq * wt[..., None]; plain += vq; ws += wt
    rec = np.where((ws > 1e-3)[..., None], s / np.maximum(ws, 1e-9)[..., None], plain * 0.25)
    c0 = np.where(fr[..., None], v, rec)
    valid = fr & (z > 0) & (z < 1e6) & np.isfinite(v).all(-1)
    return c0.astype(np.float32), raw[..., 3], valid


def reduce2(t, valid):
    """nrdReduce over 2x2 blocks: keep the nearest surface. t: (h, w, 4) rgb + z*0.01."""
    h, w = t.shape[:2]
    H, W = (h + 1) // 2, (w + 1) // 2
    tp = np.zeros((H * 2, W * 2, 4), np.float32); tp[:h, :w] = np.where(valid[..., None], t, 0.0)
    blk = tp.reshape(H, 2, W, 2, 4).transpose(0, 2, 1, 3, 4).reshape(H, W, 4, 4)
    zz = blk[..., 3]
    ok = zz > 0
    zmin = np.where(ok, zz, np.inf).min(-1)
    wt = np.where(ok, np.exp(-(zz - zmin[..., None]) / np.where(np.isfinite(zmin), zmin, 1.0)[..., None] * 16.0), 0.0)
    ws = wt.sum(-1)
    out = np.zeros((H, W, 4), np.float32)
    good = ws > 0
    out[good, :3] = (blk[good][..., :3] * wt[good][..., None]).sum(1) / ws[good][:, None]
    out[good, 3] = (zz[good] * wt[good]).sum(1) / ws[good]
    return out.astype(np.float16).astype(np.float32), out[..., 3] > 0


def upsample(lvl, shift, z):
    lh, lw = lvl.shape[:2]
    h, w = z.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    px = (xx + 0.5) / (1 << shift) - 0.5; py = (yy + 0.5) / (1 << shift) - 0.5
    bx = np.floor(px).astype(int); by = np.floor(py).astype(int)
    fx = px - bx; fy = py - by
    s = np.zeros((h, w, 3), np.float32); ws = np.zeros((h, w), np.float32)
    for ox, oy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        t = lvl[np.clip(by + oy, 0, lh - 1), np.clip(bx + ox, 0, lw - 1)]
        b = (fx if ox else 1 - fx) * (fy if oy else 1 - fy)
        wt = np.where(t[..., 3] > 0, b * np.exp(-np.abs(t[..., 3] * 100.0 - z) / np.maximum(z, 1e-3) * DEPTH_SIGMA), 0.0)
        s += t[..., :3] * wt[..., None]; ws += wt
    return np.where((ws > 1e-6)[..., None], s / np.maximum(ws, 1e-12)[..., None], 0.0), ws


def candidates(c, k):
    """Per pixel: the four candidates (h, w, 4, 3), their confidences, the hand logits, and stats."""
    c0, hit, valid = load_input(c, k)
    z = c['z']
    t = np.concatenate([c0 * 0 + c['bases'][k][..., :3], (z * 0.01)[..., None]], -1)   # pyramid: raw, fresh only
    t = np.where(valid[..., None], t, 0.0)
    l1, _ = reduce2(t.astype(np.float16).astype(np.float32), valid)
    l2, _ = reduce2(l1, l1[..., 3] > 0)
    l3, _ = reduce2(l2, l2[..., 3] > 0)
    cs, confs = [c0], [np.ones(z.shape, np.float32)]
    for i, lv in enumerate((l1, l2, l3)):
        u, cf = upsample(lv, i + 1, z)
        cs.append(u); confs.append(np.clip(cf, 0, 1))
    C = np.stack(cs, -2).astype(np.float32); conf = np.stack(confs, -1).astype(np.float32)
    logit = np.broadcast_to(BASE_LOGIT, conf.shape).copy()
    ratio = (c0 @ Y) / np.maximum(cs[2] @ Y, 1e-4)
    push = np.where((confs[2] > 0.25) & (ratio > 4.0), np.log2(np.maximum(ratio, 4.0) / 4.0), 0.0)
    logit[..., 0] -= push; logit[..., 2] += 0.5 * push; logit[..., 3] += 0.5 * push
    contact = 1.0 - np.clip(hit / np.maximum(z * 0.3, 1e-3), 0, 1)
    logit[..., 0] += 2.0 * contact; logit[..., 1] += contact
    surf = (z > 0) & (z < 1e6)
    return C, conf, logit, dict(c0=c0, hit=hit, z=z, n=c['n'], surf=surf, contact=contact, push=push > 0)


def tiles(a):
    """(h, w, ...) -> (tiles, 64, ...) over whole 8x8 tiles."""
    h, w = a.shape[:2]
    H, W = h // 8, w // 8
    a = a[:H * 8, :W * 8]
    return a.reshape((H, 8, W, 8) + a.shape[2:]).swapaxes(1, 2).reshape((H * W, 64) + a.shape[2:])


def tile_features(C, conf, st):
    """The 16 per-tile statistics the shader computes in LDS (same order)."""
    L = tiles(C @ Y)                                  # (T, 64, 4)
    m = tiles(st['surf']).astype(np.float32)          # valid surface pixels
    cnt = np.maximum(m.sum(1), 1.0)
    mean = (L * m[..., None]).sum(1) / cnt[:, None]    # (T, 4)
    eps = 1e-4 * (mean[:, 0] + mean[:, 3]) + 1e-12
    L0 = L[..., 0]
    var = ((L0 - mean[:, :1]) ** 2 * m).sum(1) / cnt
    tot = (L0 * m).sum(1) + 1e-12
    hz = tiles(st['hit'] / np.maximum(st['z'], 1e-3))
    lhz = np.log(np.maximum(hz, 0) + 1e-3)
    lhz_m = (lhz * m).sum(1) / cnt
    zt = tiles(st['z'])
    zmin = np.where(m > 0, zt, np.inf).min(1); zmax = np.where(m > 0, zt, 0).max(1)
    nt = tiles(st['n']); nmean = (nt * m[..., None]).sum(1) / cnt[:, None]
    cf = tiles(conf)
    f = np.stack([
        np.log((mean[:, 1] + eps) / (mean[:, 0] + eps)),
        np.log((mean[:, 2] + eps) / (mean[:, 0] + eps)),
        np.log((mean[:, 3] + eps) / (mean[:, 0] + eps)),
        np.log(var / (mean[:, 0] ** 2 + eps * eps) + 1e-4),
        (L0 * m * (L0 > 4 * mean[:, :1])).sum(1) / tot,
        (L0 * m * (L0 > 16 * mean[:, :1])).sum(1) / tot,
        lhz_m,
        np.sqrt(((lhz - lhz_m[:, None]) ** 2 * m).sum(1) / cnt),
        np.where(np.isfinite(zmin) & (zmax > 0), np.log(np.maximum(zmax, 1e-3) / np.maximum(np.where(np.isfinite(zmin), zmin, 1.0), 1e-3)), 0.0),
        1.0 - np.linalg.norm(nmean, axis=-1),
        m.mean(1),
        (cf[..., 1] * m).sum(1) / cnt, (cf[..., 2] * m).sum(1) / cnt, (cf[..., 3] * m).sum(1) / cnt,
        (tiles(st['contact']) * m).sum(1) / cnt,
        (tiles(st['push']).astype(np.float32) * m).sum(1) / cnt,
    ], -1).astype(np.float32)
    return f, m


def estimate(C, conf, logit, delta):
    """S with per-tile logit corrections `delta` (T, 4); inputs already tiled."""
    a = np.exp(logit + delta[:, None, :]) * conf
    A = np.maximum(a.sum(-1), 1e-12)
    return (a[..., None] * C).sum(-2) / A[..., None], a, A


def oracle(C, conf, logit, ref, m, iters=120, lam=0.02, energy=8.0):
    # Squared error alone shrinks a heavy-tailed noisy signal toward dark; the energy term holds each
    # tile's mean to the converged mean (the brightness NRD exists to keep).
    T = C.shape[0]
    d = np.zeros((T, 4), np.float32); mo = np.zeros_like(d); vo = np.zeros_like(d)
    norm = ((ref ** 2).sum(-1) * m).sum(1) + 1e-12
    rmean = ((ref @ Y) * m).sum(1) + 1e-12
    for it in range(1, iters + 1):
        S, a, A = estimate(C, conf, logit, d)
        r = (S - ref) * m[..., None]
        dS = (a / A[..., None])[..., None] * (C - S[..., None, :])          # (T, 64, 4, 3)
        g = 2.0 * np.einsum('tpc,tpkc->tk', r, dS) / norm[:, None]
        e = (r @ Y).sum(1) / rmean                                          # relative tile-mean error
        g += energy * 2.0 * e[:, None] * np.einsum('tpkc,c,tp->tk', dS, Y, m) / rmean[:, None]
        g += 2.0 * lam * d
        mo = 0.9 * mo + 0.1 * g; vo = 0.999 * vo + 0.001 * g * g
        d -= 0.05 * (mo / (1 - 0.9 ** it)) / (np.sqrt(vo / (1 - 0.999 ** it)) + 1e-8)
        d = np.clip(d, -DELTA_MAX * 0.95, DELTA_MAX * 0.95)
    return d


def load_set(paths, tile_frac):
    """Per pose: one oracle correction per tile, shared by the pose's four frames (four independent
    noise draws), so it describes the tile rather than one draw's noise. Each frame's own tile
    statistics become a training sample with that shared target."""
    F, D, keep = [], [], []
    for p in paths:
        c = nrd_io.load(p)
        ref = tiles(c['mean'][..., :3])
        fr = [candidates(c, k) for k in range(len(c['bases']))]
        feats = [tile_features(C, conf, st) for C, conf, logit, st in fr]
        sel = np.flatnonzero(feats[0][1].mean(1) > 0.5)
        if tile_frac < 1.0: sel = rng.choice(sel, int(len(sel) * tile_frac), replace=False)
        cat = lambda i: np.concatenate([tiles(x[i])[sel] for x in fr], 1)
        mt = np.concatenate([f[1][sel] for f in feats], 1)
        d = oracle(cat(0), cat(1), cat(2), np.concatenate([ref[sel]] * len(fr), 1), mt, energy=0.0)
        for k, (f, m) in enumerate(feats):
            F.append(f[sel]); D.append(d)
            keep.append((tiles(fr[k][0])[sel], tiles(fr[k][1])[sel], tiles(fr[k][2])[sel], ref[sel], m[sel]))
        print('  %s: %d tiles x %d frames' % (os.path.basename(p), len(sel), len(fr)), flush=True)
    return np.concatenate(F), np.concatenate(D), keep


class Net:
    def __init__(self):
        def he(o, i): return (rng.standard_normal((o, i)) * np.sqrt(2.0 / i)).astype(np.float32)
        self.p = {'W1': he(N_H1, N_IN), 'b1': np.zeros(N_H1, np.float32), 'W2': he(N_H2, N_H1),
                  'b2': np.zeros(N_H2, np.float32), 'W3': he(N_OUT, N_H2) * 0.1, 'b3': np.zeros(N_OUT, np.float32)}
        self.m = {k: np.zeros_like(v) for k, v in self.p.items()}; self.v = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.t = 0

    def forward(self, X):
        p = self.p
        z1 = X @ p['W1'].T + p['b1']; h1 = np.maximum(z1, 0)
        z2 = h1 @ p['W2'].T + p['b2']; h2 = np.maximum(z2, 0)
        t = np.tanh(h2 @ p['W3'].T + p['b3'])
        return DELTA_MAX * t, (X, z1, h1, z2, h2, t)

    def step(self, X, Dt, lr):
        out, (X, z1, h1, z2, h2, t) = self.forward(X)
        n = len(X)
        do = 2.0 * (out - Dt) / n * DELTA_MAX * (1 - t * t)
        g = {'W3': do.T @ h2, 'b3': do.sum(0)}
        dz2 = (do @ self.p['W3']) * (z2 > 0); g['W2'] = dz2.T @ h1; g['b2'] = dz2.sum(0)
        dz1 = (dz2 @ self.p['W2']) * (z1 > 0); g['W1'] = dz1.T @ X; g['b1'] = dz1.sum(0)
        self.t += 1
        for k in self.p:
            self.m[k] = 0.9 * self.m[k] + 0.1 * g[k]; self.v[k] = 0.999 * self.v[k] + 0.001 * g[k] ** 2
            self.p[k] -= lr * (self.m[k] / (1 - 0.9 ** self.t)) / (np.sqrt(self.v[k] / (1 - 0.999 ** self.t)) + 1e-8)
        return float(((out - Dt) ** 2).mean())


def judge(keep, deltas, name):
    """Image error of S against the converged input, and its energy ratio (finished models only)."""
    num = den = sm = rm = 0.0
    for (C, conf, logit, ref, m), d in zip(keep, deltas):
        S, _, _ = estimate(C, conf, logit, d)
        num += float((((S - ref) ** 2).sum(-1) * m).sum()); den += float(((ref ** 2).sum(-1) * m).sum())
        sm += float(((S @ Y) * m).sum()); rm += float(((ref @ Y) * m).sum())
    print('  %-9s relative L2 %.4f | energy %.4f' % (name, num / den, sm / rm))


def main():
    root, out = sys.argv[1], sys.argv[2]
    train = sorted(glob.glob(os.path.join(root, 'night', '*.bin')) + glob.glob(os.path.join(root, 'day', '*.bin')))
    held = sorted(glob.glob(os.path.join(root, 'heldout', '*.bin')))
    t0 = time.time()
    print('train', len(train)); Ft, Dt, _ = load_set(train, 0.35)
    print('held-out', len(held)); Fv, Dv, keep = load_set(held, 1.0)
    print('oracles: %.0fs, %d / %d tiles' % (time.time() - t0, len(Ft), len(Fv)))
    mu, sd = Ft.mean(0), Ft.std(0) + 1e-4
    Xt, Xv = (Ft - mu) / sd, (Fv - mu) / sd
    net = Net(); best, best_p = 1e9, None
    for ep in range(30):
        perm = rng.permutation(len(Xt)); lr = 2e-3 if ep < 15 else 7e-4 if ep < 25 else 2e-4
        tl = [net.step(Xt[perm[i:i + 1024]], Dt[perm[i:i + 1024]], lr) for i in range(0, len(perm), 1024)]
        ve = float(((net.forward(Xv)[0] - Dv) ** 2).mean())
        if ep % 3 == 2 or ep == 29: print('epoch %2d train %.4f held-out %.4f' % (ep, np.mean(tl), ve), flush=True)
        if ve < best: best, best_p = ve, {k: v.copy() for k, v in net.p.items()}
    net.p = best_p
    pred = net.forward(Xv)[0]
    sizes = np.cumsum([0] + [len(k[0]) for k in keep])
    split = lambda a: [a[sizes[i]:sizes[i + 1]] for i in range(len(keep))]
    print('held-out spatial estimate vs converged input:')
    judge(keep, split(np.zeros_like(Dv)), 'fixed')
    judge(keep, split(pred), 'network')
    judge(keep, split(Dv), 'oracle')
    # Weights file for the denoiser: "NRDW", version 1, n_in, n_h1, n_h2, n_out (u32), then float32:
    # input mean, input std, W1 (n_h1 x n_in), b1, W2, b2, W3, b3.
    with open(out, 'wb') as f:
        np.array([0x5744524E, 1, N_IN, N_H1, N_H2, N_OUT], np.uint32).tofile(f)
        for a in (mu, sd, net.p['W1'], net.p['b1'], net.p['W2'], net.p['b2'], net.p['W3'], net.p['b3']):
            np.ascontiguousarray(a, np.float32).tofile(f)
    print('weights written', out, os.path.getsize(out))


if __name__ == '__main__':
    main()
