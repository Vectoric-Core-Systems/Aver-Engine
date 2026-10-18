"""Many-light direct lighting: the numbers MANY_LIGHTS.md section 5C asks for, from captured PNGs.

    python scripts/many_lights_metrics.py compare --ref ref.png test1.png [test2.png ...] [--mask-top 70] [--linear]
    python scripts/many_lights_metrics.py flicker frames_dir_or_glob [--mask-top 70]
    python scripts/many_lights_metrics.py identical a.png b.png

compare   each test image against the reference (the exhaustive voxi.manyLights 3 path-traced still, or the legacy
          capture): mean luminance ratio (test / ref; 1.0 is unbiased), the per-block error of block-mean luminance
          (default 16x16 pixels: mean, p95 and max), the BLOB error (mean |low-passed difference| over the mean
          reference luminance, a 16 px box), the BRIGHT-SPOT area (the share of pixels more than --spot-delta above
          the reference, in 0-255 display units), MAD, and PSNR. The UNIFIED_LIGHTS.md metrics: blob error and spot area.
flicker   the mean absolute change between consecutive frames (run once with TAA on and once off; frames from
          --frames captures or a --sequence-capture directory, sorted by name).
identical byte comparison of two images (a scene at or under voxi.manyLightsMin lights must equal the legacy capture).

Rows at the top of a screenshot (the editor toolbar) are masked with --mask-top; pass the same value to every call.
Without --linear the values are the PNG's 8-bit display values; with --linear they are sRGB-decoded first, which is the
right space for a luminance ratio of a tonemapped capture (capture with --tonemap 0 for a truly linear metric).
Needs Pillow and numpy. Exit codes: 0 ok, 1 an image could not be compared, 2 usage.
"""
import argparse, glob, os, sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("many_lights_metrics.py needs Pillow and numpy (pip install pillow numpy)", file=sys.stderr)
    sys.exit(3)


def load(path, mask_top, linear):
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    if mask_top:
        img = img[mask_top:]
    if linear:
        img = np.where(img <= 0.04045, img / 12.92, ((img + 0.055) / 1.055) ** 2.4)
    return img


def luminance(img):
    return img[..., 0] * 0.2126 + img[..., 1] * 0.7152 + img[..., 2] * 0.0722


def block_means(lum, block):
    h, w = (lum.shape[0] // block) * block, (lum.shape[1] // block) * block
    v = lum[:h, :w].reshape(h // block, block, w // block, block)
    return v.mean(axis=(1, 3))


def box_blur(lum, radius):
    """Separable box blur via cumulative sums (edge-clamped)."""
    def one_axis(a, axis):
        k = 2 * radius + 1
        pad = [(0, 0)] * a.ndim
        pad[axis] = (radius + 1, radius)
        p = np.pad(a, pad, mode="edge")
        c = np.cumsum(p, axis=axis, dtype=np.float64)
        n = a.shape[axis]
        hi = np.take(c, range(k, k + n), axis=axis)
        lo = np.take(c, range(0, n), axis=axis)
        return ((hi - lo) / k).astype(np.float32)
    return one_axis(one_axis(lum, 0), 1)


def compare(ref_path, test_paths, args):
    ref = load(ref_path, args.mask_top, args.linear)
    ref_lum = luminance(ref)
    ok = True
    print(f"reference: {ref_path}  ({ref.shape[1]}x{ref.shape[0]} after masking {args.mask_top} rows, "
          f"{'linear' if args.linear else 'display'} values)")
    for tp in test_paths:
        test = load(tp, args.mask_top, args.linear)
        if test.shape != ref.shape:
            print(f"{tp}: size {test.shape[1]}x{test.shape[0]} differs from the reference -- not compared", file=sys.stderr)
            ok = False
            continue
        lum = luminance(test)
        ratio = float(lum.mean() / max(ref_lum.mean(), 1e-9))
        bm_t, bm_r = block_means(lum, args.block), block_means(ref_lum, args.block)
        berr = np.abs(bm_t - bm_r) / np.maximum(bm_r, 1.0 / 255.0)
        blob = float(np.abs(box_blur(lum - ref_lum, 8)).mean() / max(ref_lum.mean(), 1e-9))
        spot = float(((lum - ref_lum) > args.spot_delta / 255.0).mean())
        diff = np.abs(test - ref)
        mad = float(diff.mean() * 255.0)
        mse = float(((test - ref) ** 2).mean())
        psnr = 10.0 * np.log10(1.0 / mse) if mse > 0 else float("inf")
        print(f"\n{tp}")
        print(f"  mean luminance ratio  {ratio:8.4f}   (1.0000 = unbiased; the 5C.2 gate is within 1%)")
        print(f"  block error ({args.block}x{args.block})  mean {berr.mean()*100:6.2f}%  p95 {np.percentile(berr, 95)*100:6.2f}%  "
              f"max {berr.max()*100:6.2f}%")
        print(f"  blob error            {blob*100:8.3f}%   (16 px low-passed difference / mean reference luminance)")
        print(f"  bright-spot area      {spot*100:8.3f}%   (pixels more than {args.spot_delta:g}/255 above the reference)")
        print(f"  MAD {mad:7.3f} (0-255)   PSNR {psnr:6.2f} dB")
    return ok


def flicker(src, args):
    paths = sorted(glob.glob(os.path.join(src, "*.png")) if os.path.isdir(src) else glob.glob(src))
    if len(paths) < 2:
        print("flicker needs at least two frames", file=sys.stderr)
        return False
    prev = load(paths[0], args.mask_top, args.linear)
    deltas = []
    for p in paths[1:]:
        cur = load(p, args.mask_top, args.linear)
        if cur.shape != prev.shape:
            print(f"{p}: size changed -- stopped", file=sys.stderr)
            return False
        deltas.append(float(np.abs(cur - prev).mean() * 255.0))
        prev = cur
    d = np.array(deltas)
    print(f"{len(paths)} frames: frame-to-frame MAD mean {d.mean():.3f}  p95 {np.percentile(d, 95):.3f}  max {d.max():.3f}  (0-255)")
    return True


def identical(a, b):
    ba, bb = open(a, "rb").read(), open(b, "rb").read()
    if ba == bb:
        print("identical: the files are byte for byte equal")
        return True
    ia, ib = np.asarray(Image.open(a).convert("RGBA")), np.asarray(Image.open(b).convert("RGBA"))
    if ia.shape == ib.shape and np.array_equal(ia, ib):
        print("identical: the pixels are equal (the files differ in metadata only)")
        return True
    n = int((ia != ib).any(axis=-1).sum()) if ia.shape == ib.shape else -1
    print(f"DIFFERENT: {n if n >= 0 else 'sizes differ'} pixels differ")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare")
    c.add_argument("--ref", required=True)
    c.add_argument("tests", nargs="+")
    c.add_argument("--block", type=int, default=16)
    c.add_argument("--spot-delta", type=float, default=40.0, help="a spot is this many 0-255 levels above the reference")
    c.add_argument("--mask-top", type=int, default=0)
    c.add_argument("--linear", action="store_true")
    f = sub.add_parser("flicker")
    f.add_argument("frames")
    f.add_argument("--mask-top", type=int, default=0)
    f.add_argument("--linear", action="store_true")
    i = sub.add_parser("identical")
    i.add_argument("a")
    i.add_argument("b")
    args = ap.parse_args()
    if args.cmd == "compare":
        ok = compare(args.ref, args.tests, args)
    elif args.cmd == "flicker":
        ok = flicker(args.frames, args)
    else:
        ok = identical(args.a, args.b)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
