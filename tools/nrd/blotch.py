import sys, numpy as np
from PIL import Image
# Blotch metric on the GI component (variant minus giIntensity-0 frame, linear): band-pass energy at
# blob scale (box9 - box33) relative to the GI mean; single-pixel noise and broad lighting excluded.
d, off = sys.argv[1], sys.argv[2]
w = np.array([0.2126, 0.7152, 0.0722])
def lin(c): return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def load(n):
    a = np.asarray(Image.open(d + '/' + n + '.png').convert('RGB')).astype(np.float64) / 255.0
    h, wd, _ = a.shape
    return lin(a[int(h * 0.14):int(h * 0.96), 0:int(wd * 0.78)]) @ w
def box(x, r):
    k = 2 * r + 1; p = np.pad(x, r, mode='edge'); c = np.pad(p.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)
L0 = load(off)
for n in sys.argv[3:]:
    g = np.maximum(load(n) - L0, 0.0); m = g.mean()
    bp = box(g, 4) - box(g, 16)
    print('%-10s GI mean %.5f | blotch %.3f | blob-px %.2f%%' % (n, m, np.abs(bp).mean() / m, (bp > 2 * m).mean() * 100))
