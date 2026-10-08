"""A/B a level's camera path: the same fixed-step fly-through, rendered once per setting, compared frame by frame.

    python scripts/seq-ab.py <level.ocworld> --out <dir> --denoiser 0 1 2
    python scripts/seq-ab.py <level.ocworld> --out <dir> --run "ffx=--denoiser 1" --run "nrd2=--denoiser 2 --render-scale 0.5"
    python scripts/seq-ab.py --out <dir> --compare-only            # re-run only the comparison on <dir>

The level must carry a camera path (Animate mode: fly the camera, press K at each stop, save the level).
Each run is one headed, bounded Sandbox.exe process:

    Sandbox.exe <level> <common args> <run args> --no-vsync --sequence-play --sequence-capture <out>/<run> ...

so every run poses the same camera on the same frame (1 / fps per frame, no wall clock) and writes
seq_NNNNN.png, N being the frame number of the pass. Runs are named after their setting: --denoiser 0 1 2
gives none, ffx and nrd2; --run NAME=ARGS names a run and gives it any arguments (several flags allowed).
--common ARGS go to every run (e.g. "--render-scale 0.5").

After the runs, for every frame all runs have, it writes
    <out>/compare/frame_NNNNN.png   the runs side by side, and under them each run's difference to the
                                    reference run (the first, or --ref), amplified by --diff-gain
    <out>/summary.txt, summary.csv  per run and frame: mean / p99 / max pixel delta (0-255), PSNR, the share
                                    of pixels over --threshold, and ISLANDS (as shots.py diff: connected groups
                                    of tiles a real change fills and scattered noise does not)
and prints the per-run summary, including "flicker": the mean change between consecutive captured frames
(camera motion is the same in every run, so a run with a larger value is the more unstable one).

Needs Pillow and numpy. Do not run it while another Sandbox.exe is open: runs share the GPU.
"""
import argparse, csv, glob, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shots  # noqa: E402  (islands() is its tile-island finder)

DENOISER_NAMES = {0: "none", 1: "ffx", 2: "nrd2"}
FRAME_GLOB = "seq_*.png"


def parse_runs(a):
    runs = []
    for v in a.denoiser or []:
        runs.append((DENOISER_NAMES.get(v, f"denoiser{v}"), ["--denoiser", str(v)]))
    for spec in a.run or []:
        name, sep, args = spec.partition("=")
        if not sep:
            sys.exit(f"--run needs NAME=ARGS, got '{spec}'")
        runs.append((name.strip(), args.split()))
    names = [n for n, _ in runs]
    if len(set(names)) != len(names):
        sys.exit(f"run names must be unique: {names}")
    return runs


def sandbox_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Sandbox.exe"], capture_output=True, text=True).stdout
    return "Sandbox.exe" in out


def default_exe():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    for d in ("build-release", "build-wt", "build"):
        p = os.path.join(root, d, "bin", "Sandbox.exe")
        if os.path.exists(p):
            return p
    return os.path.join(root, "build-release", "bin", "Sandbox.exe")


def run_one(a, name, args):
    exe = os.path.abspath(a.exe)
    out = os.path.abspath(os.path.join(a.out, name))
    os.makedirs(out, exist_ok=True)
    for old in glob.glob(os.path.join(out, FRAME_GLOB)):
        os.remove(old)
    cmd = [exe, os.path.abspath(a.level)] + a.common.split() + args + [
        "--no-vsync", "--sequence-play", "--sequence-capture", out,
        "--sequence-every", str(a.every), "--sequence-warmup", str(a.warmup)]
    print(f"[{name}] {' '.join(cmd)}", flush=True)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=os.path.dirname(exe), timeout=a.timeout)
        log, code = r.stdout + "\n---stderr---\n" + r.stderr, r.returncode
    except subprocess.TimeoutExpired as e:
        partial = e.stdout or ""   # bytes on timeout even with text=True
        log = partial.decode(errors="replace") if isinstance(partial, bytes) else partial
        code = None
    open(os.path.join(out, "run.log"), "w", encoding="utf-8").write(log)
    n = len(glob.glob(os.path.join(out, FRAME_GLOB)))
    lost = "DEVICE HAS BEEN LOST" in log
    ok = code == 0 and n > 0 and not lost
    print(f"[{name}] {'ok' if ok else 'FAILED'}: {n} frame(s) in {time.time() - t0:.0f} s, "
          f"exit {code if code is not None else 'timeout'}{'  (device lost)' if lost else ''}", flush=True)
    if not ok:
        tail = [l for l in log.splitlines() if "SequenceRun" in l or "ERROR" in l or "error" in l][-6:]
        for l in tail:
            print("    " + l)
    return ok


def frame_index(path):
    return int(os.path.basename(path)[4:-4])


def load_run(dirpath):
    return {frame_index(p): p for p in glob.glob(os.path.join(dirpath, FRAME_GLOB))}


def metrics(x, y, threshold, block, min_island):
    import numpy as np
    d = np.abs(x - y).mean(axis=2)
    mse = float(((x - y) ** 2).mean())
    over = d > threshold
    th, tw = d.shape[0] // block, d.shape[1] // block
    tiles = over[: th * block, : tw * block].reshape(th, block, tw, block).mean(axis=(1, 3)) > 0.25
    big = [i for i in shots.islands(tiles) if i[0] >= min_island]
    return {"mean": float(d.mean()), "p99": float(np.percentile(d, 99)), "max": float(d.max()),
            "psnr": 99.0 if mse <= 1e-9 else 10.0 * float(np.log10(255.0 * 255.0 / mse)),
            "over": float(over.mean() * 100.0), "islands": len(big)}


def panel(img, label, width):
    from PIL import Image, ImageDraw
    h = max(1, round(img.height * width / img.width))
    p = img.resize((width, h), Image.LANCZOS)
    d = ImageDraw.Draw(p)
    d.rectangle([0, 0, 8 * len(label) + 10, 14], fill=(0, 0, 0))
    d.text((4, 1), label, fill=(255, 255, 255))
    return p


def side_by_side(names, imgs, ref, a, out_path):
    import numpy as np
    from PIL import Image
    top = [panel(Image.fromarray(imgs[n].astype("uint8")), n + (" (ref)" if n == ref else ""), a.panel_width) for n in names]
    bottom = []
    for n in names:
        if n == ref:
            bottom.append(Image.new("RGB", top[0].size, (24, 24, 24)))
            continue
        d = np.clip(np.abs(imgs[n] - imgs[ref]) * a.diff_gain, 0, 255)
        bottom.append(panel(Image.fromarray(d.astype("uint8")), f"|{n} - {ref}| x{a.diff_gain:g}", a.panel_width))
    w = sum(p.width for p in top)
    sheet = Image.new("RGB", (w, top[0].height + bottom[0].height))
    x = 0
    for t, b in zip(top, bottom):
        sheet.paste(t, (x, 0))
        sheet.paste(b, (x, t.height))
        x += t.width
    sheet.save(out_path)


def compare(a, names):
    import numpy as np
    from PIL import Image
    runs = {n: load_run(os.path.join(a.out, n)) for n in names}
    for n in names:
        if not runs[n]:
            sys.exit(f"no frames for run '{n}' in {os.path.join(a.out, n)}")
    ref = a.ref if a.ref in names else (names[int(a.ref)] if a.ref and a.ref.isdigit() and int(a.ref) < len(names) else names[0])
    names = [ref] + [n for n in names if n != ref]   # the reference leads every sheet
    common = sorted(set.intersection(*[set(r) for r in runs.values()]))
    if not common:
        sys.exit("the runs have no frame in common")
    skipped = {n: len(runs[n]) - len(common) for n in names}
    if any(skipped.values()):
        print("frames not in every run (left out): " + ", ".join(f"{n} {v}" for n, v in skipped.items() if v))
    cmp_dir = os.path.join(a.out, "compare")
    os.makedirs(cmp_dir, exist_ok=True)

    rows, flicker, prev = [], {n: [] for n in names}, {}
    for k, f in enumerate(common):
        imgs = {n: np.asarray(Image.open(runs[n][f]).convert("RGB"), dtype=np.float32) for n in names}
        if len({i.shape for i in imgs.values()}) != 1:
            print(f"frame {f}: image sizes differ between runs, skipped")
            continue
        for n in names:
            if n in prev:
                flicker[n].append(float(np.abs(imgs[n] - prev[n]).mean()))
            if n != ref:
                m = metrics(imgs[n], imgs[ref], a.threshold, a.block, a.min_island)
                rows.append({"frame": f, "run": n, **m})
        prev = imgs
        if k % a.compare_every == 0:
            side_by_side(names, imgs, ref, a, os.path.join(cmp_dir, f"frame_{f:05d}.png"))

    with open(os.path.join(a.out, "summary.csv"), "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=["frame", "run", "mean", "p99", "max", "psnr", "over", "islands"])
        w.writeheader()
        w.writerows(rows)

    lines = [f"reference: {ref}   frames compared: {len(common)}   threshold {a.threshold:g}   "
             f"island >= {a.min_island} tiles of {a.block}px",
             f"{'run':14s} {'mean':>7s} {'p99':>7s} {'worst max':>9s} {'PSNR dB':>8s} {'>thr %':>7s} "
             f"{'frames w/ islands':>18s} {'flicker':>8s}"]
    ref_flick = np.mean(flicker[ref]) if flicker[ref] else 0.0
    lines.append(f"{ref + ' (ref)':14s} {'':>7s} {'':>7s} {'':>9s} {'':>8s} {'':>7s} {'':>18s} {ref_flick:8.3f}")
    for n in names:
        if n == ref:
            continue
        r = [x for x in rows if x["run"] == n]
        if not r:
            continue
        worst = max(r, key=lambda x: x["mean"])
        lines.append(f"{n:14s} {np.mean([x['mean'] for x in r]):7.3f} {np.mean([x['p99'] for x in r]):7.2f} "
                     f"{max(x['max'] for x in r):9.1f} {np.mean([x['psnr'] for x in r]):8.2f} "
                     f"{np.mean([x['over'] for x in r]):7.3f} {sum(1 for x in r if x['islands']):12d}/{len(r):<5d} "
                     f"{(np.mean(flicker[n]) if flicker[n] else 0.0):8.3f}")
        lines.append(f"{'':14s} worst frame {worst['frame']} (mean {worst['mean']:.3f}, p99 {worst['p99']:.1f})")
    text = "\n".join(lines)
    open(os.path.join(a.out, "summary.txt"), "w", encoding="utf-8").write(text + "\n")
    print(text)
    print(f"side-by-side images: {cmp_dir}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("level", nargs="?", help="the .ocworld whose camera path is flown")
    p.add_argument("--out", required=True, help="output directory (one subdirectory per run)")
    p.add_argument("--exe", default=default_exe())
    p.add_argument("--denoiser", type=int, nargs="+", help="one run per value: 0 none, 1 ffx, 2 nrd2")
    p.add_argument("--run", action="append", metavar="NAME=ARGS", help="a named run with any Sandbox arguments; repeatable")
    p.add_argument("--common", default="", help="arguments for every run, e.g. \"--render-scale 0.5\"")
    p.add_argument("--every", type=int, default=1, help="capture every Nth frame")
    p.add_argument("--warmup", type=int, default=120, help="frames to settle after the level is ready, before frame 0")
    p.add_argument("--timeout", type=int, default=3600, help="seconds per run")
    p.add_argument("--ref", default=None, help="the reference run (name or index); default the first")
    p.add_argument("--threshold", type=float, default=8.0)
    p.add_argument("--block", type=int, default=8)
    p.add_argument("--min-island", type=int, default=4)
    p.add_argument("--diff-gain", type=float, default=4.0, help="amplification of the difference panels")
    p.add_argument("--panel-width", type=int, default=640)
    p.add_argument("--compare-every", type=int, default=1, help="write a side-by-side image for every Nth common frame")
    p.add_argument("--compare-only", action="store_true", help="skip the runs; compare the run directories already in --out")
    p.add_argument("--allow-running", action="store_true", help="start even if a Sandbox.exe is already running")
    a = p.parse_args()

    if a.compare_only:
        names = sorted(d for d in os.listdir(a.out) if os.path.isdir(os.path.join(a.out, d)) and load_run(os.path.join(a.out, d)))
        if len(names) < 2:
            sys.exit("--compare-only needs at least two run directories with seq_*.png in --out")
    else:
        if not a.level:
            sys.exit("a level is needed (or --compare-only)")
        runs = parse_runs(a)
        if len(runs) < 2:
            sys.exit("give at least two runs: --denoiser 0 2, or --run NAME=ARGS twice")
        if not os.path.exists(a.exe):
            sys.exit(f"{a.exe} does not exist; pass --exe")
        if not a.allow_running and sandbox_running():
            sys.exit("a Sandbox.exe is already running; close it first (runs share the GPU), or --allow-running")
        os.makedirs(a.out, exist_ok=True)
        names = [n for n, _ in runs]
        if not all(run_one(a, n, args) for n, args in runs):
            sys.exit(1)
    compare(a, names)


if __name__ == "__main__":
    main()
