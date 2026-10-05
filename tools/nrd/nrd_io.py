import numpy as np

# Reads Denoiser::captureWrite's file (modules/render.denoise/src/Denoiser.cpp).
def load(path):
    raw = open(path, 'rb').read()
    h = np.frombuffer(raw, np.uint32, 9)
    assert h[0] == 0x4344524E and h[1] == 1, 'not an NRD capture'
    w, ht, nb = int(h[2]), int(h[3]), int(h[4])
    flags = [int(x) for x in h[5:5 + nb]]
    off = 36
    def take(dtype, ch):
        nonlocal off
        n = w * ht * ch
        a = np.frombuffer(raw, dtype, n, off)
        off += n * np.dtype(dtype).itemsize
        return a.reshape(ht, w, ch) if ch > 1 else a.reshape(ht, w)
    bases = [take(np.float16, 4).astype(np.float32) for _ in range(nb)]
    z = take(np.float32, 1)
    nrm = take(np.uint32, 1)
    mean = take(np.float16, 4).astype(np.float32)
    # RGB10A2: x, y octahedral in [0,1]; z roughness
    nx = (nrm & 1023) / 1023.0; ny = ((nrm >> 10) & 1023) / 1023.0
    f = np.stack([nx * 2 - 1, ny * 2 - 1], -1)
    n3 = np.concatenate([f, (1 - np.abs(f[..., :1]) - np.abs(f[..., 1:]))], -1)
    neg = n3[..., 2] < 0
    sx = np.where(f[..., 0] >= 0, 1.0, -1.0); sy = np.where(f[..., 1] >= 0, 1.0, -1.0)
    n3[..., 0] = np.where(neg, (1 - np.abs(f[..., 1])) * sx, n3[..., 0])
    n3[..., 1] = np.where(neg, (1 - np.abs(f[..., 0])) * sy, n3[..., 1])
    n3 /= np.linalg.norm(n3, axis=-1, keepdims=True) + 1e-9
    return dict(bases=bases, flags=flags, z=z, n=n3.astype(np.float32), mean=mean, size=(w, ht))
