"""NRD training capture: one bounded editor run, visible window, lit, the project's own GI method.
usage: capture.py <Sandbox.exe> <level .ocworld> <out dir> <poses> [wander speed]
Writes <out dir>/nrd_NNN.bin (format: Denoiser::captureWrite, modules/render.denoise/src/Denoiser.cpp)."""
import os, subprocess, sys

exe, level, out, poses = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3]), int(sys.argv[4])
speed = sys.argv[5] if len(sys.argv) > 5 else "3"
os.makedirs(out, exist_ok=True)
args = [exe, level, "--render-scale", "0.5", "--frame-interp", "0", "--no-taa", "--no-neuraa",
        "--cam-wander", "1.5", speed, "--nrd-capture", out, str(poses), "--frames", str(poses * 340 + 600)]
r = subprocess.run(args, capture_output=True, text=True, cwd=os.path.dirname(exe))
open(os.path.join(out, "run.log"), "w", encoding="utf-8").write(r.stdout + "\n---stderr---\n" + r.stderr)
print("exit", r.returncode, "| captures written:", (r.stdout + r.stderr).count("] NRD capture "))
