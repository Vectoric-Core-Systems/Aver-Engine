import numpy as np

# Reads NeuRaa::captureWrite's file (modules/render.sr/src/NeuRaa.cpp).
def load(path):
    raw = open(path, 'rb').read()
    ver = int(np.frombuffer(raw, np.uint32, 2)[1])
    h = np.frombuffer(raw, np.uint32, 9 if ver == 1 else 10)
    assert h[0] == 0x4141524E and ver in (1, 2), 'not a NeuRAA capture'
    w, ht = int(h[2]), int(h[3])
    vp = [int(x) for x in h[4:8]]
    off = 36 if ver == 1 else 40
    def take(dtype, ch):
        nonlocal off
        n = w * ht * ch * np.dtype(dtype).itemsize
        a = np.frombuffer(raw, dtype, w * ht * ch, off).reshape(ht, w, ch) if ch > 1 else \
            np.frombuffer(raw, dtype, w * ht, off).reshape(ht, w)
        off += n
        return a
    base = take(np.float16, 4)[..., :3].astype(np.float32)
    ref = take(np.float16, 4)[..., :3].astype(np.float32)
    z = take(np.float32, 1)
    edges = take(np.uint8, 1)
    dist = take(np.uint8, 4).astype(np.float32) / 255.0
    res = take(np.float16, 4)[..., :3].astype(np.float32) if ver >= 2 else None
    resolved = ver >= 2 and int(h[9]) == 1
    x0, y0, vw, vh = vp
    sl = (slice(y0, y0 + vh), slice(x0, x0 + vw))
    return dict(base=base[sl], ref=ref[sl], z=z[sl], edges=edges[sl], dist=dist[sl], vp=vp, size=(w, ht),
                resolved=res[sl] if resolved else None)
