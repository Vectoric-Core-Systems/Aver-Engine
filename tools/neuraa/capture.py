"""NeuRAA training capture: one bounded editor run, visible window, Unlit, ray-driven staged.
usage: capture.py <Sandbox.exe> <level .ocworld> <out dir> <poses> [--with-neuraa]
Writes <out dir>/neuraa_NNN.bin (format: NeuRaa::captureWrite, modules/render.sr/src/NeuRaa.cpp)."""
import os, subprocess, sys

# Exit codes follow core/ErrorCodes.hpp ExitCode: 0 ok, 1 failed, 2 usage, 3 environment.
if len(sys.argv) < 5 or not sys.argv[4].isdigit():
    print(__doc__.splitlines()[1])
    sys.exit(2)  # ExitCode.Usage
exe, level, out, poses = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3]), int(sys.argv[4])
if not os.path.isfile(exe):
    print("not found:", exe)
    sys.exit(3)  # ExitCode.Environment
os.makedirs(out, exist_ok=True)
args = [exe, level, "--render-scale", "0.5", "--frame-interp", "0", "--no-taa",
        "--neuraa" if "--with-neuraa" in sys.argv else "--no-neuraa", "--unlit",
        "--rt-render-mode", "1", "--rd-stages", "2", "--cam-wander", "1.5", "3",
        "--neuraa-capture", out, str(poses), "--frames", str(poses * 140 + 600)]
r = subprocess.run(args, capture_output=True, text=True, cwd=os.path.dirname(exe))
open(os.path.join(out, "run.log"), "w", encoding="utf-8").write(r.stdout + "\n---stderr---\n" + r.stderr)
captured = (r.stdout + r.stderr).count("] capture ")
print("exit", r.returncode, "| captures written:", captured)
sys.exit(0 if r.returncode == 0 and captured > 0 else 1)  # ExitCode.Ok / Failed
