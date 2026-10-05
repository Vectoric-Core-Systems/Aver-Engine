"""End-to-end check of NeuRAA in the engine against the training script.
usage: nraa_eval.py <weights> <capture files...>"""
import sys
import numpy as np
import nraa_io, nraa_train as T


def load_weights(path):
    raw = open(path, 'rb').read()
    h = np.frombuffer(raw, np.uint32, 6)
    f = np.frombuffer(raw, np.float32, offset=24)
    n_in, h1, h2, n_out = (int(x) for x in h[2:6])
    o = 0
    def take(n):
        nonlocal o
        a = f[o:o + n]; o += n; return a
    mu, sd = take(n_in), take(n_in)
    net = T.Mlp()
    net.p = {'W1': take(h1 * n_in).reshape(h1, n_in), 'b1': take(h1), 'W2': take(h2 * h1).reshape(h2, h1),
             'b2': take(h2), 'W3': take(n_out * h2).reshape(n_out, h2), 'b3': take(n_out)}
    return mu, sd, net


mu, sd, net = load_weights(sys.argv[1])
tot = {k: [] for k in ('off', 'base', 'engine', 'python')}
mism = []
for path in sys.argv[2:]:
    c = nraa_io.load(path)
    if c['resolved'] is None: print(path, 'no resolved frame'); continue
    X, wb9, C9, m = T.features(c)
    ref = T.shift(c['ref'], 0, 0); eng = T.shift(c['resolved'], 0, 0); base = T.shift(c['base'], 0, 0)
    Xm, Wm, Cm = X[m], wb9[m], C9[m]
    w = np.concatenate([net.forward((Xm[i:i + 65536] - mu) / sd, Wm[i:i + 65536])[0] for i in range(0, len(Xm), 65536)])
    py = np.einsum('ni,nic->nc', w, Cm)
    bl = np.einsum('ni,nic->nc', Wm, Cm)
    r = ref[m]
    mism.append(np.abs(eng[m] - py).mean() / max(np.abs(r).mean(), 1e-6))
    for k, v in (('off', base[m]), ('base', bl), ('engine', eng[m]), ('python', py)):
        tot[k].append(np.abs(v - r).mean())
    # Off-edge pixels must be untouched by the engine.
    off_edge = np.abs(eng[~m] - base[~m]).max()
    print('%s: edge px %d | engine vs python %.2e (relative) | off-edge max change %.2e' % (
        path[-14:], m.sum(), mism[-1], off_edge))
print('edge colour error vs 64-sample reference (held-out):')
for k in ('off', 'base', 'engine', 'python'):
    print('  %-7s %.5f' % (k, np.mean(tot[k])))
