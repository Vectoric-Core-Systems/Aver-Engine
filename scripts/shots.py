"""Whole-frame capture and A/B diff (idea from Drift Engine's shots.mjs; see docs/TOOLING_COMPARISON_DRIFT.md).

    python scripts/shots.py capture <out-dir> [--list shots.txt] [--exe Sandbox.exe] [--frames N]
    python scripts/shots.py diff <dir-a> <dir-b> [--threshold 8] [--block 8] [--min-island 4] [--ignore x0,y0,x1,y1]

capture: one headed, bounded editor run per shot. A shot list line is `name | args...` (args as on the command
line, e.g. a level path and flags); '#' starts a comment. Without --list, the default list below is used.
Writes <name>.png and <name>.log (stdout + stderr); flags a device loss.

diff: for every <name>.png in both dirs, prints mean / p99 / max per-pixel delta (0-255, mean of RGB), the share
of pixels over --threshold, and ISLANDS: connected groups of --block x --block tiles where over a quarter of the
pixels exceed the threshold. Scattered noise (temporal jitter, ray noise) rarely fills a tile; a real regression
does. Exit code 1 when any shot has an island of at least --min-island tiles.

Needs Pillow and numpy.
"""
import argparse, os, subprocess, sys

DEFAULT_SHOTS = [
    "sponza_night | ../../../Aver Projects/PTTest/Content/Maps/NewSponza_Night.ocworld --render-scale 0.5 "
    "--frame-interp 0 --set post.autoExposure 0 --denoiser 2",
]


def parse_list(path):
    shots = []
    lines = open(path, encoding="utf-8").read().splitlines() if path else DEFAULT_SHOTS
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, _, args = line.partition("|")
        shots.append((name.strip(), args.split()))
    return shots


def capture(a):
    exe = os.path.abspath(a.exe)
    if "Sandbox.exe" in subprocess.run(["tasklist", "/FI", "IMAGENAME eq Sandbox.exe"],
                                       capture_output=True, text=True).stdout:
        sys.exit("a Sandbox.exe is already running; close it first (shots share the GPU)")
    os.makedirs(a.out, exist_ok=True)
    bad = 0
    for name, args in parse_list(a.list):
        png = os.path.abspath(os.path.join(a.out, name + ".png"))
        if os.path.exists(png):
            os.remove(png)
        cmd = [exe] + args + ["--frames", str(a.frames), "--no-vsync", "--screenshot", png]
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=os.path.dirname(exe), timeout=a.timeout)
        log = r.stdout + "\n---stderr---\n" + r.stderr
        open(os.path.join(a.out, name + ".log"), "w", encoding="utf-8").write(log)
        lost = "DEVICE HAS BEEN LOST" in log
        ok = os.path.exists(png) and not lost
        bad += not ok
        print(f"{name:24s} {'ok' if ok else 'FAILED'}{'  (device lost)' if lost else ''}  exit {r.returncode}")
    sys.exit(1 if bad else 0)


def islands(mask_tiles):
    import numpy as np
    seen = np.zeros_like(mask_tiles, dtype=bool)
    h, w = mask_tiles.shape
    out = []
    for y0 in range(h):
        for x0 in range(w):
            if not mask_tiles[y0, x0] or seen[y0, x0]:
                continue
            stack, n, box = [(y0, x0)], 0, [y0, x0, y0, x0]
            seen[y0, x0] = True
            while stack:
                y, x = stack.pop()
                n += 1
                box = [min(box[0], y), min(box[1], x), max(box[2], y), max(box[3], x)]
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w and mask_tiles[yy, xx] and not seen[yy, xx]:
                        seen[yy, xx] = True
                        stack.append((yy, xx))
            out.append((n, box))
    return sorted(out, reverse=True)


def diff(a):
    import numpy as np
    from PIL import Image
    names = sorted(f[:-4] for f in os.listdir(a.a) if f.endswith(".png") and os.path.exists(os.path.join(a.b, f)))
    if not names:
        sys.exit("no shot exists in both directories")
    fail = 0
    for name in names:
        x = np.asarray(Image.open(os.path.join(a.a, name + ".png")).convert("RGB"), dtype=np.float32)
        y = np.asarray(Image.open(os.path.join(a.b, name + ".png")).convert("RGB"), dtype=np.float32)
        if x.shape != y.shape:
            print(f"{name:24s} SIZE DIFFERS {x.shape} vs {y.shape}")
            fail = 1
            continue
        d = np.abs(x - y).mean(axis=2)
        for r in a.ignore:
            x0, y0, x1, y1 = (int(v) for v in r.split(","))
            d[y0:y1, x0:x1] = 0.0
        over = d > a.threshold
        b = a.block
        th, tw = d.shape[0] // b, d.shape[1] // b
        tiles = over[: th * b, : tw * b].reshape(th, b, tw, b).mean(axis=(1, 3)) > 0.25
        isl = islands(tiles)
        big = [i for i in isl if i[0] >= a.min_island]
        fail |= bool(big)
        print(f"{name:24s} mean {d.mean():6.3f}  p99 {np.percentile(d, 99):6.2f}  max {d.max():6.1f}  "
              f">{a.threshold}: {over.mean() * 100:6.3f}%  islands >= {a.min_island} tiles: {len(big)}")
        for n, (y0, x0, y1, x1) in big[:5]:
            print(f"    {n:5d} tiles at x {x0 * b}-{(x1 + 1) * b}, y {y0 * b}-{(y1 + 1) * b}")
    sys.exit(1 if fail else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("out")
    c.add_argument("--list")
    c.add_argument("--exe", default=os.path.join(os.path.dirname(__file__), "..", "build-release", "bin", "Sandbox.exe"))
    c.add_argument("--frames", type=int, default=600)
    c.add_argument("--timeout", type=int, default=900)
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--threshold", type=float, default=8.0)
    d.add_argument("--block", type=int, default=8)
    d.add_argument("--min-island", type=int, default=4)
    d.add_argument("--ignore", action="append", default=[], metavar="X0,Y0,X1,Y1",
                   help="pixel rect left out of the diff (UI text such as frame times); repeatable")
    a = p.parse_args()
    capture(a) if a.cmd == "capture" else diff(a)


if __name__ == "__main__":
    main()
