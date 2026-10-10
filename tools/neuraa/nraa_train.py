"""NeuRAA training (docs/rendering/NEURAA_NRD.md section 3).

1. Features per edge pixel, mirroring sr_neuraa.hlsl exactly (the engine recomputes them).
2. Oracle 3x3 weights per edge pixel: non-negative, summing to 1, fitted to the 64-sample reference by
   projected gradient, regularised toward the baseline weights. Colours are normalised per pixel.
3. An MLP (36-32-32-9, ReLU) whose bounded output adjusts the baseline's log-weights. It is trained on
   the WEIGHTS only (parameter space): no image loss is back-propagated, and model selection uses the
   held-out weight error. Image error judges the finished network only (design doc section 7).
usage: nraa_train.py <capture root> <out weights> [--train dir,dir] [--val dir]
"""
import glob, os, sys, time
import numpy as np
import nraa_io

DIRS = [(-1, 0), (1, 0), (0, -1), (0, 1)]           # edge-code bit order: left, right, up, down
NB9 = [(dx, dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1)]   # 3x3, row-major; self is index 4
DIR9 = [3, 5, 1, 7]
LUMA = np.array([0.2126, 0.7152, 0.0722], np.float32)
N_IN, N_H1, N_H2, N_OUT = 36, 32, 32, 9
DELTA_MAX = 4.0
rng = np.random.default_rng(1)


def shift(a, dx, dy):
    """a at (x + dx, y + dy) over the interior (a one-pixel border is dropped)."""
    h, w = a.shape[:2]
    return a[1 + dy:h - 1 + dy, 1 + dx:w - 1 + dx]


def baseline(z, e, dist):
    """The shader's baseline blend: weights for self, left, right, up, down (interior)."""
    Z = shift(z, 0, 0); E = shift(e, 0, 0).astype(np.uint32); D = shift(dist, 0, 0)
    ws = []
    for d, (dx, dy) in enumerate(DIRS):
        od = d ^ 1
        bit = ((E >> d) & 1) != 0
        zq = shift(z, dx, dy)
        qbit = ((shift(e, dx, dy).astype(np.uint32) >> od) & 1) != 0
        near = np.maximum(0.0, 0.5 - D[..., d])
        far = np.where(qbit, np.maximum(0.0, shift(dist, dx, dy)[..., od] - 0.5), 0.0)
        ws.append(np.where(bit, np.where(Z <= zq, near, far), 0.0))
    w = np.stack(ws, -1)
    s = w.sum(-1, keepdims=True)
    k = np.where(s > 0.75, 0.75 / np.maximum(s, 1e-9), 1.0)
    w = w * k
    return np.concatenate([1.0 - w.sum(-1, keepdims=True), w], -1)   # self, L, R, U, D


def features(c):
    base, z, e, dist = c['base'], c['z'], c['edges'], c['dist']
    E = shift(e, 0, 0).astype(np.uint32)
    bits, cls = E & 15, E >> 4
    L = base @ LUMA
    Z = np.maximum(shift(z, 0, 0), 1e-3)
    D = shift(dist, 0, 0)
    f = [D[..., d] for d in range(4)]
    f += [shift(dist, dx, dy)[..., d ^ 1] for d, (dx, dy) in enumerate(DIRS)]
    f += [((bits >> d) & 1).astype(np.float32) for d in range(4)]
    f += [(cls == k).astype(np.float32) for k in (1, 2, 3)]
    f += [((shift(e, dx, dy).astype(np.uint32) >> (d ^ 1)) & 1).astype(np.float32) for d, (dx, dy) in enumerate(DIRS)]
    f += [np.clip(np.log(np.maximum(shift(z, dx, dy), 1e-3) / Z), -2.0, 2.0) for dx, dy in DIRS]
    Lp = shift(L, 0, 0) + 1e-3
    f += [np.clip(np.log((shift(L, dx, dy) + 1e-3) / Lp), -3.0, 3.0) for dx, dy in NB9 if (dx, dy) != (0, 0)]
    wb = baseline(z, e, dist)
    f += [wb[..., i] for i in range(5)]
    X = np.stack(f, -1).astype(np.float32)
    wb9 = np.zeros(wb.shape[:2] + (9,), np.float32)
    wb9[..., 4] = wb[..., 0]
    for d in range(4): wb9[..., DIR9[d]] = wb[..., d + 1]
    C9 = np.stack([shift(base, dx, dy) for dx, dy in NB9], -2)   # (h, w, 9, 3)
    return X, wb9, C9, bits != 0


def project_simplex(v):
    """Euclidean projection of each row onto the probability simplex."""
    u = -np.sort(-v, 1)
    css = np.cumsum(u, 1) - 1.0
    idx = np.arange(1, v.shape[1] + 1)
    cond = u - css / idx > 0
    rho = cond.shape[1] - 1 - np.argmax(cond[:, ::-1], 1)
    theta = css[np.arange(len(v)), rho] / (rho + 1)
    return np.maximum(v - theta[:, None], 0.0)


def oracle(C9, r, wb9, lam=0.02, iters=150):
    s = (C9 @ LUMA).mean(1, keepdims=True) + 1e-3                 # per-sample scale
    Cn = C9 / s[..., None]; rn = r / s
    A = np.einsum('nic,njc->nij', Cn, Cn) + lam * np.eye(9)[None]   # normal equations, 9x9 per sample
    b = np.einsum('nic,nc->ni', Cn, rn) + lam * wb9
    step = 1.0 / (np.linalg.norm(A, axis=(1, 2)) + 1e-6)
    w = wb9.copy()
    for _ in range(iters):
        g = np.einsum('nij,nj->ni', A, w) - b
        w = project_simplex(w - step[:, None] * g)
    return w.astype(np.float32)


def load_set(paths, per_capture):
    Xs, Ws, Ts, Cs, Rs = [], [], [], [], []
    for p in paths:
        c = nraa_io.load(p)
        X, wb9, C9, m = features(c)
        r = shift(c['ref'], 0, 0)
        idx = np.flatnonzero(m.ravel())
        if len(idx) > per_capture: idx = rng.choice(idx, per_capture, replace=False)
        X = X.reshape(-1, N_IN)[idx]; wb = wb9.reshape(-1, 9)[idx]
        C = C9.reshape(-1, 9, 3)[idx]; R = r.reshape(-1, 3)[idx]
        Xs.append(X); Ws.append(wb); Cs.append(C); Rs.append(R)
        Ts.append(oracle(C, R, wb))
        print('  %s: %d edge samples' % (os.path.basename(p), len(idx)), flush=True)
    return [np.concatenate(a) for a in (Xs, Ws, Ts, Cs, Rs)]


class Mlp:
    def __init__(self):
        def he(o, i): return (rng.standard_normal((o, i)) * np.sqrt(2.0 / i)).astype(np.float32)
        self.p = {'W1': he(N_H1, N_IN), 'b1': np.zeros(N_H1, np.float32),
                  'W2': he(N_H2, N_H1), 'b2': np.zeros(N_H2, np.float32),
                  'W3': (he(N_OUT, N_H2) * 0.1), 'b3': np.zeros(N_OUT, np.float32)}
        self.m = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.v = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.t = 0

    def forward(self, X, wb):
        p = self.p
        z1 = X @ p['W1'].T + p['b1']; h1 = np.maximum(z1, 0)
        z2 = h1 @ p['W2'].T + p['b2']; h2 = np.maximum(z2, 0)
        o = h2 @ p['W3'].T + p['b3']; t = np.tanh(o)
        l = np.log(wb + 1e-3) + DELTA_MAX * t
        l -= l.max(1, keepdims=True); w = np.exp(l); w /= w.sum(1, keepdims=True)
        return w, (X, z1, h1, z2, h2, t)

    def step(self, X, wb, T, lr):
        w, (X, z1, h1, z2, h2, t) = self.forward(X, wb)
        n = len(X)
        dw = 2.0 * (w - T) / n
        dl = w * (dw - (dw * w).sum(1, keepdims=True))
        do = dl * DELTA_MAX * (1.0 - t * t)
        g = {'W3': do.T @ h2, 'b3': do.sum(0)}
        dz2 = (do @ self.p['W3']) * (z2 > 0)
        g['W2'] = dz2.T @ h1; g['b2'] = dz2.sum(0)
        dz1 = (dz2 @ self.p['W2']) * (z1 > 0)
        g['W1'] = dz1.T @ X; g['b1'] = dz1.sum(0)
        self.t += 1
        for k in self.p:
            self.m[k] = 0.9 * self.m[k] + 0.1 * g[k]
            self.v[k] = 0.999 * self.v[k] + 0.001 * g[k] * g[k]
            mh = self.m[k] / (1 - 0.9 ** self.t); vh = self.v[k] / (1 - 0.999 ** self.t)
            self.p[k] -= lr * mh / (np.sqrt(vh) + 1e-8)
        return float(((w - T) ** 2).sum(1).mean())

    def weight_error(self, X, wb, T):
        err = 0.0
        for i in range(0, len(X), 65536):
            w, _ = self.forward(X[i:i + 65536], wb[i:i + 65536])
            err += float(((w - T[i:i + 65536]) ** 2).sum(1).sum())
        return err / len(X)


def colour_error(w, C, R):
    return float(np.abs(np.einsum('ni,nic->nc', w, C) - R).mean())


def main():
    root, out = sys.argv[1], sys.argv[2]
    # Scene folders under the root: --train a,b --val c (default: v1's nd_day,sponza / cyber).
    opt = {k: v.split(',') for k, v in zip(sys.argv[3::2], sys.argv[4::2]) if k in ('--train', '--val')}
    pick = lambda dirs: sorted(f for d in dirs for f in glob.glob(os.path.join(root, d, '*.bin')))
    train = pick(opt.get('--train', ['nd_day', 'sponza']))
    val = pick(opt.get('--val', ['cyber']))
    t0 = time.time()
    print('train captures', len(train)); Xt, Wt, Tt, Ct, Rt = load_set(train, 60000)
    print('held-out captures', len(val)); Xv, Wv, Tv, Cv, Rv = load_set(val, 60000)
    print('data + oracle: %.0fs, %d train / %d held-out samples' % (time.time() - t0, len(Xt), len(Xv)))
    mu = Xt.mean(0); sd = Xt.std(0) + 1e-4
    Xtn = (Xt - mu) / sd; Xvn = (Xv - mu) / sd

    net = Mlp()
    best, best_p = 1e9, None
    for epoch in range(12):
        perm = rng.permutation(len(Xtn))
        lr = 2e-3 if epoch < 6 else 7e-4 if epoch < 10 else 2e-4
        tl = []
        for i in range(0, len(perm), 4096):
            b = perm[i:i + 4096]
            tl.append(net.step(Xtn[b], Wt[b], Tt[b], lr))
        ve = net.weight_error(Xvn, Wv, Tv)
        print('epoch %2d  train weight err %.5f  held-out weight err %.5f' % (epoch, np.mean(tl), ve), flush=True)
        if ve < best: best, best_p = ve, {k: v.copy() for k, v in net.p.items()}
    net.p = best_p

    # Judging the finished network only: colour error against the reference, on held-out edge pixels.
    wnet = np.concatenate([net.forward(Xvn[i:i + 65536], Wv[i:i + 65536])[0] for i in range(0, len(Xvn), 65536)])
    none = np.zeros_like(Wv); none[:, 4] = 1.0
    print('held-out weight err: baseline %.5f  network %.5f' % (float(((Wv - Tv) ** 2).sum(1).mean()), best))
    print('held-out edge colour error vs 64-sample reference:')
    for name, w in (('no AA', none), ('baseline', Wv), ('network', wnet), ('oracle', Tv)):
        print('  %-9s %.5f' % (name, colour_error(w, Cv, Rv)))

    # Weights file read by NeuRaa (render.sr): "NRAW", version 1, n_in, n_h1, n_h2, n_out (u32), then
    # float32: input mean, input std, W1 (row-major, n_h1 x n_in), b1, W2, b2, W3, b3.
    with open(out, 'wb') as f:
        np.array([0x5741524E, 1, N_IN, N_H1, N_H2, N_OUT], np.uint32).tofile(f)
        for a in (mu, sd, net.p['W1'], net.p['b1'], net.p['W2'], net.p['b2'], net.p['W3'], net.p['b3']):
            np.ascontiguousarray(a, np.float32).tofile(f)
    print('weights written:', out, os.path.getsize(out), 'bytes')


if __name__ == '__main__':
    main()
