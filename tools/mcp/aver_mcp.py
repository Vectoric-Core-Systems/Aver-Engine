#!/usr/bin/env python3
"""An MCP server for driving the Aver engine.

WHY THIS EXISTS. Every visual defect found in this tree was found by building, launching Sandbox.exe
with flags, screenshotting, cropping and looking -- nine of them in one phase (docs/STATUS.md 4u), and
three more in the session that wrote this file. That loop is entirely mechanical and I have run it by
hand a dozen times. This turns it into tools an agent can call, so "open it and look" stops being the
step that gets skipped because it is tedious.

THE BATCH TOOLS DRIVE THE EXISTING CLI AND CHANGE NOTHING IN THE ENGINE. The hosts already take the
flags -- --frames, --screenshot, --probe-rel, --open-asset, --force-caps, --warp and the rest -- and the
gates oracle is built entirely out of them. So aver_build, aver_run, aver_tests, aver_gates,
aver_package and aver_flags need no engine-side listener, socket or named pipe, and cannot disturb a
working editor. That was the deliberate first cut: a live control channel is a real feature with
threading and lifetime concerns, and it should not be the thing that also introduces this tooling.

THE LIVE CHANNEL CAME LATER, as modules/mcp (McpBridge), and aver_editor / aver_level are its client:
one TCP connection per call to 127.0.0.1, into an editor the USER started with --mcp or from the
status bar. They never start an editor or the channel, and they say how to when nothing is listening.

The count used to be written out here ("already takes 43 flags"). It was 43 when that sentence was
written and 183 when someone next checked, which is the whole reason aver_flags reads the set out of
the source instead of listing it: a number in prose is a claim nobody re-derives. There is no count in
this file any more, deliberately.

THERE ARE TWO HOSTS, and they do NOT parse the same flags. Sandbox.exe is the editor
(sandbox/src/SandboxMain.cpp); AverEngineRuntime.exe is the shipped game host
(Runtime/src/GameApp.cpp::parseArgs, Runtime/host/). The runtime parses no render-override flag at
all -- no --rt, no --gi, no --force-caps, no --probe/--probe-rel -- so the gates oracle can only ever
be driven against the editor. Both hosts IGNORE an argument they do not recognise rather than
rejecting it (GameApp.cpp says so in as many words: "Anything else is deliberately ignored"), which is
why aver_run names the host it ran and reports any flag it could not find in that host's source.

WHAT IT WILL NOT DO, and these are refusals rather than omissions:

  * It never runs `gates.ps1 -Record`. Re-recording overwrites the only record of what the renderer
    used to do; it is the user's call, and scripts/record-gates.ps1 is how they make it.
  * It never launches an interactive Sandbox.exe. --frames is always passed, so every run terminates on
    its own. A windowed editor left running holds bin/*.dll open and the next build fails LNK1168 --
    and the user may have their own editor open, which this must not disturb.
  * It never kills a process it did not start.
  * aver_level edits only the level the running editor has open, and refuses while it is playing.

TRANSPORT: two, both PURE STDLIB -- no mcp package, no npm install, nothing to provision on a fresh
machine and nothing whose licence has to be vetted against this repo's permissive-only rule.

  1. stdio (default, unchanged). JSON-RPC 2.0, newline-delimited, over stdin/stdout. This is what
     .mcp.json launches today (`{"command":"python","args":["tools/mcp/aver_mcp.py"]}`) and it is not
     going anywhere -- plain `python aver_mcp.py`, no arguments, is byte-for-byte the same loop it
     always was. main() below IS that loop.

  2. HTTP (opt-in: pass --http), a.k.a. MCP "Streamable HTTP". Same JSON-RPC messages, POSTed to
     /mcp as `application/json`, one request in and one response out -- proved sufficient against a
     real client (Claude Code) without also implementing the SSE-push half of the spec: a stray GET
     just gets a 405, and a client that never needed a server-initiated push tolerates that fine. See
     serve_http() below. Chosen because a bare TCP socket speaking this file's own line-delimited
     JSON-RPC is NOT one of the transports an MCP client can actually dial (checked: `claude mcp add
     --help` lists exactly stdio, sse, http) -- so "give it a port" has to mean HTTP or it would be an
     unreachable transport, which is worse than no transport at all.

  Both transports dispatch through the SAME handle(msg) -- the wire format changes, the protocol and
  every tool's behaviour do not.
"""
import base64
import http.server
import json
import os
import re
import socket
import subprocess
import sys
import time

# The repo root: this file lives at tools/mcp/, so that is THREE levels up, not two. Two was the first
# version and it put ROOT at tools/ -- every PowerShell call would have run `./scripts/build.ps1` from
# the wrong directory. Found because the tool-level exception handler reported it as a result instead of
# taking the server down with it.
#
# Asserted rather than assumed, because a wrong ROOT fails confusingly: `build.ps1 not found` sends a
# reader looking for a missing script rather than a miscounted dirname.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if not os.path.isfile(os.path.join(ROOT, "CMakeLists.txt")):
    sys.stderr.write("[aver-mcp] FATAL: %s is not the repo root (no CMakeLists.txt)\n" % ROOT)
    sys.exit(2)

SERVER_NAME = "aver-engine"
SERVER_VERSION = "0.1.0"

# stdout is the protocol channel; anything that is not a JSON-RPC message corrupts the stream. So all
# diagnostics go to stderr, and print() is never used.
def log(msg):
    sys.stderr.write("[aver-mcp] %s\n" % msg)
    sys.stderr.flush()


# --------------------------------------------------------------------------------------------------
# mcp.conf: optional, developer-local port assignment. Same file, same grammar, as the one
# sandbox/src/McpConf.cpp reads for the editor's --mcp control channel (see mcp.conf.example at the
# repo root) -- this is the Python-side reader: tool_server.port for this server's own HTTP port, and
# editor_bridge.port for where aver_editor and aver_level look for the editor's channel. Kept as a
# free function rather than folded into ROOT's module-level setup so it can be called with an
# explicit root/default in a test without touching the real mcp.conf.
# --------------------------------------------------------------------------------------------------

def _parse_strict_port(v):
    """Parses `v` as a port the same way the C++ reader's std::from_chars does: ASCII digits only,
    the whole (already-trimmed) string, nothing else -- no leading '+', no '_' digit-group
    separators, no leading/trailing junk. Returns an int or None.

    Plain int(v) is NOT equivalent: Python's int() accepts a leading '+' ("+8080") and PEP-515
    underscore grouping ("8_080") that std::from_chars rejects outright. Both readers' docstrings
    claim "the same contract" as each other; before this, that claim was false for exactly those two
    inputs -- the same mcp.conf line was accepted here and rejected (with a warning) on the C++ side,
    silently giving the editor and the tool server two different ports from one file. Caught by
    actually running both parsers on the same adversarial input, not by re-reading either one.
    """
    if not re.fullmatch(r"[0-9]+", v):
        return None
    return int(v)


def read_conf_port(root, key, default):
    """Reads `key` from <root>/mcp.conf as a TCP port (1-65535).

    Same optional, per-key contract as the C++ reader: a missing file, an empty file, or a missing
    key all fall back to `default` SILENTLY -- that is the normal case, not a problem. A key that IS
    present but does not parse (not a number, out of range, trailing junk) also falls back to
    `default`, but logs why, because that one is a typo a developer would otherwise never see.

    Grammar: one `key = value` per line, '#' and blank lines ignored, first occurrence of a
    duplicate key wins (matching or not -- the first match is authoritative either way, same as the
    C++ reader). Whitespace around the key and the value is trimmed: mcp.conf is meant to be
    hand-edited from mcp.conf.example, where every line is aligned with spaces around '=', so this is
    deliberately more forgiving than editor.ini's own machine-written-and-read grammar.

    Returns (port, source) where source is "mcp.conf" or "built-in default" (the latter possibly
    annotated with why, for the log line).
    """
    path = os.path.join(root, "mcp.conf")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return default, "built-in default"     # no file: the ordinary, silent case

    for raw in text.splitlines():
        row = raw.rstrip("\r")
        stripped = row.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in row:
            continue                            # a line with no '=': skipped, not fatal
        k, _, v = row.partition("=")
        k = k.strip()
        if k != key:
            continue
        v = v.strip()
        port = _parse_strict_port(v)
        if port is None or not (1 <= port <= 65535):
            log("mcp.conf: %r = %r is not a usable port (1-65535); falling back to default %s"
                % (key, v, default))
            return default, "built-in default (mcp.conf value invalid)"
        return port, "mcp.conf"                 # first occurrence wins
    return default, "built-in default"          # key not present: the ordinary, silent case


def resolve_tool_server_port(argv):
    """Explicit `--port` argument > mcp.conf's tool_server.port > built-in default (8787).

    Read once at startup and logged. Only actually BINDS anything when the process is also told
    --http (see main()/serve_http()) -- over stdio (still the default; see .mcp.json) the resolved
    value is reported but never opens a socket.
    """
    default = 8787
    cli_port = None
    for i, a in enumerate(argv):
        if a == "--port" and i + 1 < len(argv):
            cli_port = argv[i + 1]
        elif a.startswith("--port="):
            cli_port = a.split("=", 1)[1]
    if cli_port is not None:
        # Same strict digit-only grammar as read_conf_port, for the same reason: consistency between
        # this server's own two entry points into the same number, not just with the C++ reader.
        p = _parse_strict_port(cli_port)
        if p is not None and 1 <= p <= 65535:
            return p, "command line"
        log("--port %r is not a usable port (1-65535); ignoring, falling back to mcp.conf/default"
            % cli_port)
    return read_conf_port(ROOT, "tool_server.port", default)


def run_powershell(script, timeout):
    """Run a PowerShell command from the repo root and return (exit, stdout+stderr)."""
    proc = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        cwd=ROOT, capture_output=True, text=True, timeout=timeout, errors="replace")
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


# --------------------------------------------------------------------------------------------------
# tools
# --------------------------------------------------------------------------------------------------

# WHICH CONFIG THIS BUILT, SAID OUT LOUD, and an unknown argument refused rather than dropped.
#
# `release` is a BOOLEAN and always has been. A caller who says config="Release" -- a spelling this
# tool never had -- used to get silence: the unknown key was ignored, build.ps1's -Config defaulted
# to Debug, and cmake was handed -DCMAKE_BUILD_TYPE=Debug for whatever tree build_dir named. Point
# that at build-release and it does not just build the wrong thing, it RECONFIGURES the cache, so
# every later build and every measurement taken from that tree is Debug until someone notices.
# Nobody noticed for a whole session: an unoptimised binary is not slow enough to be obviously
# wrong, and the tool's own output said "[build] OK -> build-release\bin", which is true and
# useless. It also explains a Release-only compile error in modules/rhi.vulkan surviving unseen --
# scripts/module-matrix.ps1 hard-codes CMAKE_BUILD_TYPE=Debug too, so nothing in this repo's normal
# workflow produces an NDEBUG build at all.
#
# So: `config` is accepted as the string spelling, anything else is REFUSED by name, and the result
# always reports the config and tree that were actually used. Same principle as aver_run's
# binary_provenance -- a result that does not say what it did cannot be told from one that did the
# wrong thing.
_BUILD_KNOWN_ARGS = {"release", "config", "build_dir", "cmake_args", "target", "description"}
_BUILD_CONFIGS = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")


def default_tree_for(config):
    """The build tree scripts/build.ps1 picks for `config` when -BuildDir is not given.

    Copied from that script's own default rather than guessed, because this file used to guess and
    got it wrong for half the configs: `"build-release" if config != "Debug" else "build"` reported
    build-release for RelWithDebInfo and MinSizeRel, whose real trees are build-relwithdebinfo and
    build-minsizerel. That is the same class of defect as the dropped `config` the comment above
    describes -- a result that names a tree the build never touched -- only quieter, because the
    build genuinely succeeded and only the report was false.
    """
    return "build" if config == "Debug" else "build-" + config.lower()


def tool_build(args):
    unknown = sorted(k for k in args if k not in _BUILD_KNOWN_ARGS)
    if unknown:
        return {"ok": False, "error": "unknown argument(s) %s -- this tool takes release (bool), "
                                      "config (%s), build_dir, cmake_args. Refused rather than "
                                      "ignored, because a dropped config silently builds Debug."
                                      % (", ".join(unknown), "|".join(_BUILD_CONFIGS))}
    config = args.get("config")
    if config is not None:
        match = [c for c in _BUILD_CONFIGS if c.lower() == str(config).lower()]
        if not match:
            return {"ok": False, "error": "config %r is not one of %s" % (config, ", ".join(_BUILD_CONFIGS))}
        config = match[0]
    elif args.get("release"):
        config = "Release"
    else:
        config = "Debug"
    # `target` WAS ACCEPTED AND SILENTLY DROPPED, which is the exact failure the comment above this
    # block is about, committed a second time. scripts/build.ps1 hands everything after its own
    # parameters to scripts/build.bat, which forwards them to CMAKE CONFIGURE (`%*` on the -S/-B
    # line) and then always runs a bare `cmake --build "%AVER_BUILD_DIR%"` with no --target. So a
    # target name could not reach the build step even in principle: it would have landed on the
    # configure line, where CMake takes an unknown bare argument as a source-directory-ish positional
    # and warns rather than failing. Refused by name instead, because building everything when the
    # caller asked for one target is indistinguishable in the result from building that one target.
    if args.get("target"):
        return {"ok": False, "error": "target %r cannot be honoured: scripts/build.ps1 forwards extra "
                                      "arguments to CMake CONFIGURE and then builds the whole tree "
                                      "(scripts/build.bat has no --target). Drop it, or run cmake "
                                      "--build <tree> --target <t> from a developer shell."
                                      % args["target"]}
    extra = args.get("cmake_args") or []
    build_dir = args.get("build_dir")
    if build_dir and not re.fullmatch(r"[A-Za-z0-9_.-]+", build_dir):
        # The SAME charset check resolve_tree and tool_package already apply, which this call site
        # did not: build_dir is interpolated straight into the PowerShell command string below, so a
        # quote or a semicolon in it ran as script. The three are meant to agree; only two did.
        return {"ok": False, "error": "suspicious build_dir %r: letters, digits, dot, dash and "
                                      "underscore only" % build_dir}
    # -Config rather than -Release even for Release, and that is not an oversight: build.ps1 defines
    # -Release as nothing more than a default for -Config ($Config = if ($Release) {'Release'}), and
    # -BuildDir derives from $Config either way, so the two spellings produce an identical build. Only
    # -Config can also say RelWithDebInfo or MinSizeRel, so one code path says it for all four.
    cmd = "./scripts/build.ps1 -Config %s" % config
    if build_dir:
        cmd += ' -BuildDir "%s"' % build_dir
    for a in extra:
        cmd += " " + a
    code, out = run_powershell(cmd, timeout=1800)

    # The whole log is thousands of lines of Ninja progress. What a caller needs is whether it built and
    # what went wrong, so errors and warnings are extracted and the rest is dropped.
    errors = [l.strip() for l in out.splitlines() if re.search(r"\berror\b [A-Z]+\d+|error:", l)]
    warnings = [l.strip() for l in out.splitlines() if re.search(r"\bwarning [A-Z]+\d+", l)]
    tail = [l.strip() for l in out.splitlines() if l.strip().startswith("[build]")]
    return {
        "ok": code == 0,
        "exit": code,
        "config": config,
        "build_dir": build_dir or default_tree_for(config),
        "status": tail[-1] if tail else "(no [build] line)",
        "errors": errors[:40],
        "warning_count": len(warnings),
    }


# Which build tree aver_run / aver_tests reach into.
#
# WHY THIS EXISTS AT ALL. aver_build has always taken build_dir; aver_run and aver_tests hardcoded
# "build"/"build-release". That asymmetry is worse than it sounds, because agents in this repo are
# told to build into their OWN tree and never touch `build` -- so they could compile a change and
# then had no way to run it. aver_run silently launched `build/bin/Sandbox.exe` instead: a DIFFERENT,
# older binary that does not have their change and does not know their new flag, which an unknown
# flag being ignored rather than rejected makes completely silent. A whole perf investigation
# reported four frame-time numbers for a flag the binary it measured had never heard of, and only
# caught it by noticing the timestamps. Numbers like that are worse than no numbers.
#
# release + build_dir together: build_dir wins, because it is the more specific statement of intent.
# `release` only ever selected a tree NAME, and naming the tree outright says the same thing better.
def resolve_tree(args):
    """Returns (tree_name, error_or_None). Same charset check tool_package uses.

    `release` covers only the two trees this repo actually pairs baselines with (build and
    build-release, see scripts/build.ps1's header). A RelWithDebInfo or MinSizeRel tree has to be
    named outright with build_dir -- there is no third boolean, because a run against one of those
    has no gate baseline to mean anything against.
    """
    build_dir = args.get("build_dir")
    if build_dir:
        # Rejects path separators and .., so this can only ever name a sibling of the repo root --
        # the argument is interpolated into a filesystem path below.
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", build_dir):
            return None, ("suspicious build_dir %r: letters, digits, dot, dash and underscore only, "
                          "and it must be a directory directly under the repo root" % build_dir)
        return build_dir, None
    return ("build-release" if bool(args.get("release")) else "build"), None


# THE TWO HOSTS, by the names they have since the editor/runtime split (2026-09-16).
#
# `game` was the old third entry and it is gone: AverGame.exe lived in game/ with its library in
# modules/runtime.game, and both paths were deleted when the runtime moved to Runtime/. The library
# is Aver.Runtime.Game.Core (shared) plus Aver.Runtime.Game (GameApp alone), and the executable is
# AverEngineRuntime.exe -- the FILE has no spaces so nothing has to quote it; the name people see is
# "Aver Engine Runtime", set by the .rc. See Runtime/host/CMakeLists.txt.
#
# The source roots are what binary_provenance walks and what aver_flags reads, so they are stated
# once here rather than in each.
_HOSTS = {
    "editor": {
        "exe": "Sandbox.exe",
        # sandbox/src/Sandbox*.{cpp,hpp} only. Verified rather than assumed: those are the only files
        # under sandbox/src containing a "--flag" string literal at all -- the sole other hits in the
        # directory are ProjectScaffold.cpp's "--" and "---", which are markdown rules, not flags.
        "flag_globs": [("sandbox/src", lambda n: n.startswith("Sandbox") and n.endswith((".cpp", ".hpp")))],
    },
    "runtime": {
        "exe": "AverEngineRuntime.exe",
        "flag_globs": [("Runtime/src", lambda n: n.endswith((".cpp", ".hpp"))),
                       ("Runtime/host", lambda n: n.endswith((".cpp", ".hpp")))],
    },
}


def resolve_host(args):
    """Returns (host_key, exe_basename, error_or_None). Defaults to the editor.

    AverEngineRuntime.exe only exists in a tree configured with AVER_BUILD_GAME=ON (the default, see
    the root CMakeLists.txt). A tree built without it reports the missing file rather than falling
    back to Sandbox.exe, because silently measuring the other host is the same wrong-binary failure
    resolve_tree exists to stop.
    """
    host = args.get("host") or "editor"
    if host not in _HOSTS:
        return None, None, ("unknown host %r: one of %s" % (host, ", ".join(sorted(_HOSTS))))
    return host, _HOSTS[host]["exe"], None


def host_flag_set(host):
    """Every "--flag" string literal in that host's own sources, as a set.

    A HEURISTIC, and used as one: it finds literals, not parse sites, so a flag only mentioned in a
    log line would be counted as accepted. It cannot miss a parsed flag on either host today --
    both parse with std::strcmp against a literal and neither uses strncmp or starts_with (checked)
    -- and over-accepting is the safe direction for the only thing it is used for, which is telling
    a caller that a flag they passed appears nowhere in the host that was about to ignore it.
    """
    flags = set()
    for rel, keep in _HOSTS[host]["flag_globs"]:
        base = os.path.join(ROOT, *rel.split("/"))
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if not keep(name):
                continue
            with open(os.path.join(base, name), encoding="utf-8", errors="replace") as f:
                flags.update(re.findall(r'"(--[a-z0-9-]+)"', f.read()))
    return flags


# The flags this refuses to pass through, and why. A denylist rather than an allowlist because the CLI
# grows and an allowlist would silently block new flags.
#
# "these two are the only ones that can outlive the call" is what this comment used to say, and it
# described neither the contents nor the reason: there has only ever been ONE entry, and --headless
# does not outlive anything -- it is refused because a run with no swapchain cannot write the
# screenshot this tool exists to produce. Both hosts take --headless, so the refusal applies to both.
_REFUSED_FLAGS = {
    "--headless": "pointless here -- a headless run cannot produce the screenshot this tool exists for",
}

# WHAT A RUN FROM HERE DOES *NOT* SEE: THE USER'S editor.ini.
#
# When this server is hosted inside a packaged (MSIX) app, every process it spawns inherits that
# package identity, and Windows redirects %LOCALAPPDATA% for the whole subtree. Sandbox.exe then
# reads and writes
#     AppData/Local/Packages/<package>/LocalCache/Local/AverEngine/editor.ini
# instead of AppData/Local/AverEngine/editor.ini -- a copy-on-write shadow of the real file.
#
# It is a nasty one because NOTHING LOOKS WRONG. The engine logs the unredirected path it asked
# for ("[Prefs] 37 setting(s) from C:/Users/.../AppData/Local/AverEngine/editor.ini"), the key
# count matches, and the shadow starts life as a byte-for-byte copy. It only diverges once the two
# are edited apart -- and then a preference the user has set is simply absent from every run made
# here, with no warning and no diff to notice.
#
# It cost an hour: a stored render scale that killed the editor on the user's machine could not be
# reproduced through this tool, and the same binary with the same argv reproduced it instantly from
# a shell. The shell is outside the package; this is not.
#
# SO: ANYTHING THAT DEPENDS ON A PERSISTED EDITOR PREFERENCE MUST BE RUN FROM A SHELL, not from
# here. Flags are unaffected -- they are argv, and argv is not redirected -- which is why the gates,
# which pass every setting they care about explicitly, are sound.
def tool_run(args):
    frames = int(args.get("frames", 40))
    if frames <= 0:
        return {"ok": False, "error": "frames must be positive: an interactive run would never exit, "
                                      "would hold bin/*.dll open (LNK1168 on the next build), and might "
                                      "collide with the user's own editor"}
    flags = list(args.get("flags") or [])
    for f in flags:
        if f in _REFUSED_FLAGS:
            return {"ok": False, "error": "%s refused: %s" % (f, _REFUSED_FLAGS[f])}

    tree, err = resolve_tree(args)
    if err:
        return {"ok": False, "error": err}
    host, exe_name, err = resolve_host(args)
    if err:
        return {"ok": False, "error": err}
    exe = os.path.join(ROOT, tree, "bin", exe_name)
    if not os.path.exists(exe):
        return {"ok": False, "error": "%s does not exist -- build first%s"
                % (exe, "" if host == "editor" else
                   " (the runtime host needs a tree configured with AVER_BUILD_GAME=ON)")}

    shot = args.get("screenshot")
    argv = [exe, "--frames", str(frames)]
    if shot:
        shot = os.path.abspath(shot)
        os.makedirs(os.path.dirname(shot), exist_ok=True)
        argv += ["--screenshot", shot]
    argv += flags

    # Read before the run, not after, so a build racing this call cannot change the answer between
    # the two. Only leading-dashes tokens are checked: a value ("no-rt" after --force-caps), a level
    # path and a .ocproject are all ordinary positional arguments.
    known = host_flag_set(host)
    not_parsed = [f for f in flags if f.startswith("--") and f.split("=", 1)[0] not in known]

    started = time.time()
    try:
        proc = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True,
                              timeout=int(args.get("timeout", 600)), errors="replace")
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "the run did not finish in time; it was NOT killed by force -- "
                                      "check for a windowed instance still open"}
    out = (proc.stdout or "") + (proc.stderr or "")
    lines = out.splitlines()

    # The probe line, parsed rather than handed back raw. This is the engine's own self-validating
    # measurement -- raw codes, the viewport rect, and an in/outside tag -- and it is what the gates are
    # built on. A caller that had to re-grep it would re-implement the parse in every call site.
    probe = None
    for l in lines:
        if "probe (" in l:
            m = re.search(r"raw \(([\d, ]+)\)", l)
            rect = re.search(r"viewport \((\d+),(\d+) (\d+)x(\d+)\)", l)
            tag = re.search(r"(in-viewport|OUTSIDE-VIEWPORT|VIEWPORT-MOVED)", l)
            probe = {
                "raw": re.sub(r"\s", "", m.group(1)) if m else None,
                "viewport": ("%s,%s %sx%s" % rect.groups()) if rect else None,
                "placement": tag.group(1) if tag else None,
            }
            break

    grep = args.get("grep")
    matched = [l.strip() for l in lines if re.search(grep, l)] if grep else []
    # Tags come from levelTag() in modules/core/src/Log.cpp, printed as "[TAG ] message".
    # "[CRIT" was added when the Critical level was: without it, the one severity that means "this
    # process may be about to die" was the only one that appeared in NO summary here -- it matched
    # neither the error nor the warning pattern and vanished silently from every gate run.
    errors = [l.strip() for l in lines if "[ERROR" in l or "[CRIT" in l or "[FATAL" in l]
    warns = [l.strip() for l in lines if "[WARN" in l]

    return {
        "ok": proc.returncode == 0,
        "exit": proc.returncode,
        "seconds": round(time.time() - started, 1),
        "probe": probe,
        "screenshot": shot if (shot and os.path.exists(shot)) else None,
        "errors": errors[:30],
        "warnings": warns[:30],
        "matched": matched[:60],
        "log_lines": len(lines),
        "host": host,
        # WHICH BINARY THIS ACTUALLY MEASURED, always reported, never on request. A result that does
        # not say what it ran is indistinguishable from a result that ran the wrong thing, and the
        # engine ignores unrecognized flags rather than rejecting them, so "my new flag did nothing"
        # and "my new flag was never in this binary" produce identical output. See resolve_tree.
        "binary": binary_provenance(exe),
        # THE SAME SILENCE, ONE STEP EARLIER: a flag the OTHER host parses. binary_provenance catches
        # a flag newer than the exe; this catches a flag that was never this host's to begin with.
        # The runtime parses no render-override flag at all, so `--gi --no-rt --probe-rel .5 .5`
        # aimed at AverEngineRuntime.exe runs cleanly, exits 0, prints no probe line and means
        # nothing -- and before this, said so nowhere.
        "flags_not_in_host_source": not_parsed,
    }


# Describes the binary a run actually used, and says so out loud when source is newer than it.
#
# The staleness test is deliberately a HEURISTIC, and reported as one: a source file newer than the
# executable means the executable cannot contain that edit, which is sound, but the converse proves
# nothing (an unrelated edit also trips it). A false "stale" costs a rebuild; a false "fresh" costs a
# published number that was never real, which is the failure this is here to prevent.
def binary_provenance(exe):
    # THE ROOTS IT WALKS ARE THE POINT, and one of them had been dead for days while another was
    # never added. `game/` went with AverGame.exe: os.path.isdir skipped it silently, so the list
    # read as three roots and behaved as two. `Runtime/` is where the runtime library moved, and
    # since the editor/runtime split Sandbox.exe LINKS it -- content, level, water, streaming,
    # landscape, physics/audio/tick, camera, mouse capture, input publishing and the whole world
    # draw walk including the depth prepass are all Aver.Runtime.Game.Core now. So an edit to
    # Runtime/src/GameRender.cpp is an edit to the editor binary, and until this line it could not
    # mark one stale: exactly the "published a number that was never real" failure below, aimed at
    # the code most likely to be under edit this month.
    built = os.path.getmtime(exe)
    newest, newest_path = 0.0, None
    for sub in ("modules", "sandbox", "Runtime"):
        base = os.path.join(ROOT, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            # Vendored third-party trees are huge and are not what a caller just edited.
            dirnames[:] = [d for d in dirnames if d not in ("Jolt", "third_party", ".git")]
            for fn in filenames:
                if fn.endswith((".cpp", ".hpp", ".h", ".hlsl", ".cs")):
                    p = os.path.join(dirpath, fn)
                    try:
                        m = os.path.getmtime(p)
                    except OSError:
                        continue
                    if m > newest:
                        newest, newest_path = m, os.path.relpath(p, ROOT)
    info = {
        "path": os.path.relpath(exe, ROOT),
        "built": time.strftime("%H:%M:%S", time.localtime(built)),
    }
    if newest_path and newest > built:
        info["stale"] = True
        info["newer_source"] = newest_path
        info["newer_source_at"] = time.strftime("%H:%M:%S", time.localtime(newest))
        info["warning"] = ("this binary predates %s -- it cannot contain that edit, and any flag added "
                           "with it will be silently ignored rather than rejected. Rebuild into this "
                           "tree, or pass build_dir to name the tree you actually built."
                           % newest_path)
    return info


def tool_inspect_image(args):
    """Crop and/or downscale a PNG so a region is actually legible when viewed.

    Cropping matters more than it sounds: the backbuffer is 3532x1987, and a toolbar viewed at full
    frame is a few pixels tall. Every UI defect found in this tree was found in a crop.
    """
    try:
        from PIL import Image
    except ImportError:
        return {"ok": False, "error": "Pillow is not installed; cannot crop"}

    src = os.path.abspath(args["path"])
    if not os.path.exists(src):
        return {"ok": False, "error": "%s does not exist" % src}
    im = Image.open(src).convert("RGB")
    full = im.size

    box = args.get("box")
    if box:
        x, y, w, h = (int(v) for v in box)
        # Clamped rather than rejected: an off-by-a-few crop request should give a slightly smaller
        # image, not an error that sends the caller back to guess again.
        x = max(0, min(x, full[0] - 1)); y = max(0, min(y, full[1] - 1))
        w = max(1, min(w, full[0] - x)); h = max(1, min(h, full[1] - y))
        im = im.crop((x, y, x + w, y + h))

    scale = float(args.get("scale", 1.0))
    if scale != 1.0:
        # NEAREST, so nothing is invented by the resample. A smoothed crop of a one-pixel UI seam is a
        # crop that no longer shows the seam.
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.NEAREST)

    dst = args.get("out") or (os.path.splitext(src)[0] + "_crop.png")
    dst = os.path.abspath(dst)
    im.save(dst)
    return {"ok": True, "path": dst, "size": list(im.size), "source_size": list(full)}


# THE EXACT PATTERN CTest REGISTERS, repeated here only to explain it -- see the FAIL_REGULAR_EXPRESSION
# in the root CMakeLists.txt's aver_register_ctest_in, which is the one that decides.
#
# Two spaces after FAIL, and `FAILED ===`, because the bare word is not a failure signal in this tree:
# suites describe their negative cases in prose ("an OBJ with no faces FAILS rather than returning an
# empty mesh") and a naive /FAIL/ marks six green suites red. That is the same over-match the old
# per-suite `failures` list here produced.
_CTEST_FAIL_PATTERN = re.compile(r"FAIL  |FAILED ===")

# `N/M Test #K: SuiteName ......   Passed    0.05 sec`, or `...***Failed  Required regular expression
# not found. Regex=[...]  0.12 sec`. Parsed in two steps rather than one regex because the status text
# is free-form -- Passed, Failed, Timeout, `Exception: SegFault`, and the fail-regex explanation --
# and a single pattern that tried to enumerate it would drop the row it most needs to report.
_CTEST_ROW = re.compile(r"^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+(.*)$")


def _parse_ctest_rows(lines):
    rows = []
    for raw in lines:
        m = _CTEST_ROW.match(raw)
        if not m:
            continue
        rest = m.group(2)
        secs = re.search(r"([\d.]+)\s+sec\s*$", rest)
        if secs:
            rest = rest[:secs.start()]
        status = rest.lstrip(". ").lstrip("*").strip() or "?"
        rows.append({"suite": m.group(1),
                     "status": status,
                     "seconds": float(secs.group(1)) if secs else None})
    return rows


def tool_tests(args):
    """Run the suites THROUGH CTest, which is the only way their pass/fail means anything.

    THIS USED TO GLOB bin/*Test.exe AND RUN EACH ONE DIRECTLY, keying off nothing but the exit code
    -- and that made it strictly worse than no harness. The root CMakeLists.txt registers every
    *Test target with a FAIL_REGULAR_EXPRESSION precisely because suites in this tree have
    historically returned 0 however they went; there is a commit named "Two skin tests that exited 0
    however they went", and docs/STALE_CODE.md records an `ok_` flag that was set and never read. A
    suite that prints `FAIL  ` and exits 0 was reported here as a pass, in green, by the one tool an
    agent session could reach. The old code even COLLECTED the FAIL lines into a `failures` field and
    then never let them affect the verdict.

    Nothing is lost by going through CTest: the registration walk matches exactly the set this used
    to glob -- 140 registered tests against 140 bin/*Test.exe, no name in either that is not in the
    other -- and it adds the fail-regex, the 600s per-suite timeout this tool had on its own, and the
    bin/ working directory, which this tool did NOT have. It ran from the repo root, while several
    suites resolve fixtures relative to the executable the way a shipped editor resolves shaders.
    Those suites were being run in conditions no other caller uses.

    It shells out to scripts/test.ps1 rather than to ctest directly so the ctest DISCOVERY lives in
    one place: that script falls back to Visual Studio's bundled copy when ctest is not on PATH,
    which it is not in a plain python subprocess on this machine. Its exit codes are 0 pass, 1 at
    least one suite failed, 3 environment (no tree, or a tree configured without CTest registration).
    """
    tree, err = resolve_tree(args)
    if err:
        return {"ok": False, "error": err}
    if not os.path.isdir(os.path.join(ROOT, tree)):
        return {"ok": False, "error": "%s does not exist -- build first" % os.path.join(ROOT, tree)}

    # `only` IS A CTest NAME REGEX NOW, not the case-insensitive substring it was. Said out loud in
    # the schema too, because "import" used to match ImportTest and no longer does: CMake's regex
    # engine has no inline case-insensitivity to paper over it with, and quietly matching nothing
    # would report "0 suites, all passed" -- the same shape of lie this whole rewrite is about.
    only = args.get("only") or ""
    if only and not re.fullmatch(r"[A-Za-z0-9_.|^$()\[\]*+-]+", only):
        return {"ok": False, "error": "suspicious `only` %r: it reaches a shell, so it is restricted "
                                      "to the characters a CTest -R name pattern needs" % only}

    cmd = "./scripts/test.ps1 -BuildDir %s" % tree
    if only:
        # SINGLE quotes: a CTest name pattern legitimately contains '|', '$' and '^', every one of
        # which PowerShell acts on inside double quotes -- '|' would end the command outright. A
        # single-quoted PowerShell string is literal, and the charset check above already forbids the
        # apostrophe that would close it.
        cmd += " -Filter '%s'" % only
    if args.get("rerun"):
        cmd += " -Rerun"
    jobs = args.get("jobs")
    if jobs is not None:
        if not isinstance(jobs, int) or jobs <= 0:
            return {"ok": False, "error": "jobs must be a positive integer"}
        cmd += " -Jobs %d" % jobs
    # The whole sweep, not one suite: 140 suites at test.ps1's default parallelism. The old 600s was
    # a PER-SUITE budget and CTest still applies that one per suite, so this is only the outer bound.
    code, out = run_powershell(cmd, timeout=int(args.get("timeout", 3600)))
    lines = out.splitlines()

    rows = _parse_ctest_rows(lines)
    failing = [r for r in rows if r["status"] != "Passed"]
    return {
        # test.ps1's own contract, not ctest's raw number -- it deliberately does not pass ctest's
        # vocabulary (8 = some tests failed) through, because this repo's error-code table gives 8 a
        # different meaning entirely.
        "ok": code == 0,
        "exit": code,
        "environment_error": code == 3,
        "build_dir": tree,
        "suites": len(rows),
        "failed": len(failing),
        "failures": failing[:40],
        # The lines that tripped the fail-regex, quoted. A suite can now fail for a reason its exit
        # code never carried, so the result has to say which line did it or the verdict is unarguable
        # with.
        "fail_lines": [l.strip() for l in lines if _CTEST_FAIL_PATTERN.search(l)][:40],
        "summary": next((l.strip() for l in reversed(lines) if "tests passed" in l), ""),
        "status": next((l.strip() for l in reversed(lines) if l.strip().startswith("[test]")), ""),
    }


def tool_gates(args):
    """A READ-ONLY gate sweep. -Record is not reachable from here, by design -- see the module docstring.

    ALWAYS THE EDITOR. gates.ps1's -Exe defaults to <tree>/bin/Sandbox.exe and this tool does not
    override it, which is not a limitation to be lifted: the oracle is built on --probe-rel and
    --force-caps, and AverEngineRuntime.exe parses neither. The check that the runtime draws what the
    editor draws is a different mechanism and a coarser one -- scripts/verify-game.ps1's divergence
    gate opens the same project in both hosts and compares their [Census] lines, --scene-census being
    a flag both of them do parse.

    No build_dir either, for the same reason it has no -Exe here: -Release switches the executable
    and the baseline file TOGETHER, and a third tree would have no baseline to be measured against.
    """
    configs = args.get("configs") or ["baseline"]
    bad = [c for c in configs if not re.fullmatch(r"[a-z0-9,=-]+", c)]
    if bad:
        return {"ok": False, "error": "suspicious configuration name(s): %s" % bad}
    cmd = "./scripts/gates.ps1 -Config %s" % ",".join(configs)
    if args.get("release"):
        cmd += " -Release"
    code, out = run_powershell(cmd, timeout=3000)
    lines = out.splitlines()
    return {
        "ok": code == 0,
        "failures": code,
        "gates": [l.strip() for l in lines if re.search(r"raw\(", l)],
        "invariants": [l.strip() for l in lines if "INVARIANT" in l],
        "verdict": next((l.strip() for l in reversed(lines) if "GATE" in l and
                         ("PASS" in l or "FAILED" in l)), ""),
        "note": "read-only. To re-record, a human runs ./scripts/record-gates.ps1",
    }


def tool_package(args):
    """Stage the ENGINE payload, and optionally verify it.

    THIS USED TO PACKAGE A GAME, and every call to it failed once that path was deleted: it shelled
    out to ./scripts/stage-game.ps1 and ./scripts/verify-game.ps1, which went with AverGame.exe. The
    engine payload -- the thing the launcher pulls and installs -- is what packaging means here now,
    so this points at stage-payload.ps1 / verify-payload.ps1 instead of failing on a missing file.

    NO `project` ARGUMENT ANY MORE, and that is the substantive difference rather than a rename: a
    game package was cut per-project, while a payload is the engine itself and there is no project to
    name. A caller still passing `project` is told plainly rather than having it ignored.

    STAGING IS NOT DESTRUCTIVE to the repo -- it copies into an output directory the caller names --
    but it DOES overwrite that directory when -Force is passed, so `force` stays opt-in. The two
    scripts stay separate for the same reason as before: staging produces a payload, verifying proves
    it runs somewhere else, and a tool that always did both would make it awkward to look at the
    output in between.
    """
    out = args.get("out") or ""
    if not out:
        return {"ok": False, "error": "'out' is required (the directory to stage the payload into)"}
    if args.get("project"):
        # THE GAME PATH EXISTS AGAIN as of 2026-09-05, so this refusal no longer says "there is no
        # such thing" -- it says "not through this tool". Staging a game runs a project's content
        # through stage-game.ps1 and then verify-game.ps1's divergence gate, which launches BOTH
        # hosts; that is a long, interactive-shaped operation with a real pass/fail of its own, and
        # wrapping it behind an argument to the payload tool would hide which of the two was run.
        return {"ok": False,
                "error": "this tool stages the ENGINE payload, not a game. To package a project run "
                         "./scripts/stage-game.ps1 -Project <x.ocproject> -Out <dir>, then "
                         "./scripts/verify-game.ps1 -Package <dir>, which also runs the divergence "
                         "gate against the editor. Drop 'project' and pass only 'out'."}
    if '"' in out or "`" in out or ";" in out:
        return {"ok": False, "error": "suspicious character in 'out'"}

    # Release by default, because a payload is what ships. stage-payload.ps1 derives its own build
    # directory from the config when none is given, so `build_dir` stays optional rather than being
    # defaulted here to a tree that may not exist.
    build_dir = args.get("build_dir") or ""
    if build_dir and not re.fullmatch(r"[A-Za-z0-9_.-]+", build_dir):
        return {"ok": False, "error": "suspicious build_dir"}

    cmd = './scripts/stage-payload.ps1 -Out "%s"' % out
    if build_dir:
        cmd += " -BuildDir %s" % build_dir
    cmd += " -Config %s" % (args["config"] if args.get("config") in ("Debug", "Release") else "Release")
    if args.get("with_samples"):
        cmd += " -WithSamples"
    if args.get("force"):
        cmd += " -Force"
    code, out_text = run_powershell(cmd, timeout=900)
    lines = [l.strip() for l in out_text.splitlines() if l.strip().startswith("[stage]")]
    result = {
        "ok": code == 0,
        "errors": code,
        "stage": lines,
        "verdict": next((l for l in reversed(lines) if "OK ->" in l or "FAILED" in l), ""),
    }
    if code != 0 or not args.get("verify"):
        return result

    vcode, vtext = run_powershell('./scripts/verify-payload.ps1 -Payload "%s"' % out, timeout=900)
    vlines = [l.strip() for l in vtext.splitlines() if l.strip().startswith("[verify]")]
    result["ok"] = vcode == 0
    result["verify"] = vlines
    result["verify_errors"] = vcode
    result["verify_verdict"] = next((l for l in reversed(vlines) if "OK --" in l or "FAILED" in l), "")
    return result


def tool_flags(args):
    """Each host's CLI flags, read out of its own source so this cannot go stale.

    IT USED TO RETURN ONE LIST, which read as "the engine's flags" and was the editor's alone. That
    was true enough while there was one host; since the editor/runtime split there are two, and they
    parse overlapping-but-different sets -- AverEngineRuntime.exe has no render-override flag at all,
    and has --project, --width, --height, --title, --trace-opens, --input-echo and --no-mouse-capture
    that the editor does not. Handing a caller a single merged list would have them pass an editor
    flag to the runtime, which ignores it in silence (GameApp.cpp: "Anything else is deliberately
    ignored") and produces a clean exit 0 that measured nothing.

    So: per host, plus the differences spelled out, because the differences are the part a caller
    gets wrong. See host_flag_set for why this is a literal scan and what that cannot promise.
    """
    per_host = {h: sorted(host_flag_set(h)) for h in _HOSTS}
    editor, runtime = set(per_host["editor"]), set(per_host["runtime"])
    return {
        "ok": True,
        "hosts": {h: {"exe": _HOSTS[h]["exe"], "count": len(per_host[h]), "flags": per_host[h]}
                  for h in per_host},
        "both": sorted(editor & runtime),
        "editor_only": sorted(editor - runtime),
        "runtime_only": sorted(runtime - editor),
        "refused": {k: v for k, v in _REFUSED_FLAGS.items()},
    }


# --------------------------------------------------------------------------------------------------
# aver_editor / aver_level: a client of the RUNNING editor's control channel (modules/mcp, McpBridge).
#
# Everything above launches a fresh process, runs N frames and exits. These two talk to an editor a
# person already has open -- started with --mcp, or Start on the MCP button of its status bar -- and
# change what it is showing. The wire is one JSON object per line over TCP to 127.0.0.1; the ABIs a
# request can name are registered in sandbox/src/SandboxMcp.cpp (registerMcpAbis), and "level" is the
# one that assembles a level out of the editor's own spawn, transform, delete and save code.
# --------------------------------------------------------------------------------------------------

_EDITOR_HOST = "127.0.0.1"          # the bridge binds loopback only, hard-coded (McpBridge::start)
_EDITOR_PORT_DEFAULT = 45123        # McpBridge::start's own default; mcp.conf's editor_bridge.port beats it
_EDITOR_CALL_TIMEOUT = 90.0         # the bridge itself gives up on the editor's main thread after 60s
_EDITOR_OPEN_TIMEOUT = 300.0        # a level open, load included
_editor_seq = [0]


class EditorError(Exception):
    """The editor could not be reached, or answered in a way no request should get."""


def resolve_editor_port(args):
    """An explicit `port` > mcp.conf's editor_bridge.port > 45123: the order the editor resolves the
    number a bare `--mcp` opens (sandbox/src/SandboxMain.cpp), so both sides agree from one file."""
    port = args.get("port")
    if port is not None:
        if isinstance(port, bool) or not isinstance(port, int) or not (1 <= port <= 65535):
            raise EditorError("port %r is not a usable port (1-65535)" % (port,))
        return port, "argument"
    return read_conf_port(ROOT, "editor_bridge.port", _EDITOR_PORT_DEFAULT)


def editor_send(port, message, timeout):
    """One request, one reply, one connection. Returns the reply as a dict, or raises EditorError.

    A NEW CONNECTION PER CALL, closed straight after: the bridge serves a single client and accepts the
    next only when the current one disconnects, so a connection kept open would lock out every other
    caller (a second agent, this server's own next call) until this process died.
    """
    _editor_seq[0] += 1
    full = {"id": _editor_seq[0]}
    full.update(message)
    try:
        # allow_nan=False: the bridge's number reader would refuse NaN and Infinity anyway, with a
        # message that does not say which argument it was.
        line = (json.dumps(full, allow_nan=False) + "\n").encode("ascii")
    except ValueError as e:
        raise EditorError("request holds a value JSON cannot carry: %s" % e)

    try:
        sock = socket.create_connection((_EDITOR_HOST, port), timeout=min(timeout, 5.0))
    except ConnectionRefusedError:
        raise EditorError("no editor is listening on %s:%d -- start the editor with --mcp (or --mcp <port>), "
                          "or enable MCP in its status bar (the MCP button, then Start). The port is "
                          "mcp.conf's editor_bridge.port, else %d, unless this call names one."
                          % (_EDITOR_HOST, port, _EDITOR_PORT_DEFAULT))
    except OSError as e:
        raise EditorError("could not connect to the editor at %s:%d: %s" % (_EDITOR_HOST, port, e))

    with sock:
        try:
            sock.settimeout(timeout)
            sock.sendall(line)
            buf = b""
            while b"\n" not in buf:
                chunk = sock.recv(65536)
                if not chunk:
                    raise EditorError("the editor closed the connection without answering")
                buf += chunk
        except socket.timeout:
            raise EditorError("the editor accepted the connection but did not answer within %gs -- another "
                              "client may be holding the channel (it serves one at a time), or the editor "
                              "is busy on something long (a level load?)" % timeout)
        except OSError as e:
            raise EditorError("the connection to the editor failed: %s" % e)
    try:
        return json.loads(buf.split(b"\n", 1)[0].decode("utf-8", errors="replace"))
    except json.JSONDecodeError as e:
        raise EditorError("the editor's reply is not JSON: %s" % e)


def editor_abi(port, module, fn, abi_args=None, text=None, timeout=_EDITOR_CALL_TIMEOUT):
    """An `abi` request. KEY ORDER IS PART OF THE PROTOCOL: the bridge's reader takes a key at its
    first occurrence in the line, so `text` -- arbitrary content -- goes last, after every key the
    reader looks up by name."""
    msg = {"cmd": "abi", "module": module, "fn": fn}
    if abi_args:
        msg["args"] = list(abi_args)
    if text:
        msg["text"] = text
    return editor_send(port, msg, timeout)


def _decode_result(value):
    """An ABI's result is a string; the `level` ABI's are JSON documents, so those come back parsed and
    the plain-text ones (`world`, `graph`) stay as they are."""
    if isinstance(value, str) and value[:1] in ("{", "["):
        try:
            return json.loads(value)
        except json.JSONDecodeError:
            return value
    return value


def _is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and v == v and abs(v) != float("inf")


_EDITOR_CMDS = ("abi", "ping", "modules", "widgets", "shot", "move", "click", "key", "text")


def tool_editor(args):
    known = {"cmd", "module", "fn", "args", "text", "extra", "port", "timeout"}
    unknown = sorted(k for k in args if k not in known)
    if unknown:
        return {"ok": False, "error": "unknown argument(s) %s -- this tool takes %s. Refused rather than "
                                      "ignored." % (", ".join(unknown), ", ".join(sorted(known)))}
    cmd = args.get("cmd", "abi")
    if cmd not in _EDITOR_CMDS:
        return {"ok": False, "error": "cmd %r is not one of %s" % (cmd, ", ".join(_EDITOR_CMDS))}

    msg = {"cmd": cmd}
    text = args.get("text")
    if text is not None and not isinstance(text, str):
        return {"ok": False, "error": "text must be a string"}
    if cmd == "abi":
        module, fn = args.get("module"), args.get("fn")
        if not isinstance(module, str) or not module or not isinstance(fn, str) or not fn:
            return {"ok": False, "error": "an abi request needs module and fn (ask cmd=modules for the "
                                          "registered modules)"}
        msg["module"], msg["fn"] = module, fn
        abi_args = args.get("args") or []
        if not isinstance(abi_args, list) or not all(_is_number(v) for v in abi_args):
            return {"ok": False, "error": "args must be an array of numbers (a flag is 0 or 1, not a boolean)"}
        if abi_args:
            msg["args"] = abi_args
    else:
        extra = args.get("extra") or {}
        if not isinstance(extra, dict) or any(not isinstance(k, str) or k in ("id", "cmd") for k in extra):
            return {"ok": False, "error": "extra must be an object of the command's own fields (x, y, "
                                          "button, widget, key, path), never id or cmd"}
        msg.update(extra)
    if text:
        msg["text"] = text

    try:
        port, source = resolve_editor_port(args)
        timeout = float(args["timeout"]) if "timeout" in args else _EDITOR_CALL_TIMEOUT
        if not timeout > 0:
            raise ValueError("timeout must be positive")
        reply = editor_send(port, msg, timeout)
    except (ValueError, EditorError) as e:
        return {"ok": False, "error": str(e)}
    out = {"ok": bool(reply.get("ok")), "cmd": cmd, "port": port, "port_source": source, "reply": reply}
    if "result" in reply:
        out["result"] = _decode_result(reply["result"])
    if not out["ok"]:
        out["error"] = reply.get("error", "the editor refused with no reason")
    return out


# The ops the "level" ABI answers (sandbox/src/SandboxMcp.cpp, SandboxApp::mcpLevelAbi), and the
# parameters each takes. An unknown parameter is REFUSED, like aver_build's unknown arguments: a
# misspelt `rotation` that was dropped would place the object facing the wrong way and say nothing.
_LEVEL_PARAMS = {
    "open": {"path", "discard"},
    "info": set(),
    "list": {"filter", "max"},
    "place": {"placements"},
    "set_transform": {"id", "position", "rotation", "scale"},
    "set_material": {"id", "material"},
    "set_collide": {"id", "collide"},
    "set_visible": {"id", "visible"},
    "set_anim": {"id", "clip", "speed", "time", "once"},
    "remove": {"ids"},
    "select": {"ids", "frame"},
    "save": {"path"},
    "player_start": {"position", "yaw"},
}
_LEVEL_COMMON = {"op", "port", "timeout"}


def _level_id(v, name="id"):
    if isinstance(v, float) and v.is_integer():
        v = int(v)
    if isinstance(v, bool) or not isinstance(v, int) or v < 1:
        raise ValueError("%s must be an entity id: a whole number >= 1 (got %r)" % (name, v))
    return v


def _level_vec(a, key, n, what):
    v = a.get(key)
    if not isinstance(v, (list, tuple)) or len(v) != n or not all(_is_number(x) for x in v):
        raise ValueError("%s must be an array of %d numbers: %s" % (key, n, what))
    return list(v)


def _level_flag(v, name):
    if isinstance(v, bool):
        return 1 if v else 0
    if v in (0, 1):
        return int(v)
    raise ValueError("%s must be true or false" % name)


def _level_number(a, key, default):
    v = a.get(key, default)
    if not _is_number(v):
        raise ValueError("%s must be a number" % key)
    return v


def _level_request(op, a):
    """Maps an aver_level call onto the ABI's (fn, args, text), or raises ValueError saying what is
    wrong with it. Everything the editor would refuse for a malformed value is caught here first, so
    the message names the parameter the caller actually wrote."""
    if op == "open":
        path = a.get("path")
        if not isinstance(path, str) or not path.strip():
            raise ValueError("open needs path: the level file, relative to the project's Content "
                             "directory (Maps/Arena.ocworld, or just Arena) or absolute")
        # discard THROWS AWAY UNSAVED EDITS, so it is read as a flag and never by truthiness: the string
        # "false" (or "no", or 2) is refused instead of being taken as yes. null counts as not given.
        discard = a.get("discard")
        flag = 0 if discard is None else _level_flag(discard, "discard")
        return "open", ([1] if flag else []), path.strip()
    if op == "info":
        return "info", [], None
    if op == "list":
        flt = a.get("filter", "")
        if not isinstance(flt, str):
            raise ValueError("filter must be a string")
        mx = a.get("max")
        if mx is not None and (isinstance(mx, bool) or not isinstance(mx, int) or mx < 1):
            raise ValueError("max must be a whole number >= 1")
        return "list", ([mx] if mx else []), flt
    if op == "place":
        p = a.get("placements")
        if isinstance(p, str):
            p = p.split("\n")
        if not isinstance(p, list) or not all(isinstance(x, str) for x in p):
            raise ValueError("placements must be a string of PLACE lines, or an array of them")
        lines = [x.strip() for x in p if x.strip()]
        if not lines:
            raise ValueError("placements holds no line, e.g. PLACE Meshes/cube.ocmesh 0 0 0  0 0 0  100")
        return "place", [], "\n".join(lines)
    if op == "set_transform":
        pos = _level_vec(a, "position", 3, "x, y, z in centimetres, world space")
        rot = _level_vec(a, "rotation", 3, "yaw, pitch, roll in degrees")
        abi = [_level_id(a.get("id"))] + pos + rot
        if a.get("scale") is not None:
            abi += _level_vec(a, "scale", 3, "sx, sy, sz")
        return "set_transform", abi, None
    if op == "set_material":
        mat = a.get("material")
        if not isinstance(mat, str) or not mat.strip():
            raise ValueError("set_material needs material: an .ocmat stem such as M_Wood")
        return "set_material", [_level_id(a.get("id"))], mat.strip()
    if op == "set_collide":
        return "set_collide", [_level_id(a.get("id")), _level_flag(a.get("collide"), "collide")], None
    if op == "set_visible":
        return "set_visible", [_level_id(a.get("id")), _level_flag(a.get("visible"), "visible")], None
    if op == "set_anim":
        clip = a.get("clip", "")
        if not isinstance(clip, str):
            raise ValueError("clip must be a string: a content-relative .ocanim path, or \"\" to clear")
        abi = [_level_id(a.get("id")), _level_number(a, "speed", 1.0), _level_number(a, "time", 0.0),
               _level_flag(a.get("once", False), "once")]
        return "set_anim", abi, clip.strip()
    if op == "remove":
        ids = a.get("ids")
        if not isinstance(ids, list) or not ids:
            raise ValueError("remove needs ids: an array of one or more entity ids")
        return "remove", [_level_id(i, "ids[]") for i in ids], None
    if op == "select":
        ids = a.get("ids", [])
        if not isinstance(ids, list):
            raise ValueError("ids must be an array of entity ids (empty clears the selection)")
        return "select", [_level_id(i, "ids[]") for i in ids], ("" if a.get("frame", True) else "noframe")
    if op == "save":
        path = a.get("path")
        if path is not None and not isinstance(path, str):
            raise ValueError("path must be a string: a name, or a path relative to Content or absolute")
        return "save", [], (path or "")
    if op == "player_start":
        pos = _level_vec(a, "position", 3, "x, y, z in centimetres")
        return "player_start", pos + [_level_number(a, "yaw", 0.0)], None
    raise ValueError("unhandled op %r" % op)   # unreachable: tool_level checks _LEVEL_PARAMS first


def _same_path(a, b):
    def norm(p):
        return os.path.normcase(os.path.normpath(p)) if isinstance(p, str) and p else ""
    return norm(a) != "" and norm(a) == norm(b)


def _wait_for_open(port, want, timeout):
    """`open` only QUEUES the load (the editor drains it on its own frame, past its unsaved-changes
    check), so this asks `info` until the level it names is the one open and nothing is loading.
    Returns (info, None) or (last_info_or_None, why_not)."""
    deadline = time.time() + timeout
    mismatch, prompts, info, last_err = 0, 0, None, None
    while time.time() < deadline:
        try:
            rep = editor_abi(port, "level", "info", timeout=15.0)
        except EditorError as e:
            last_err = str(e)            # the editor is busy loading: keep asking
            time.sleep(0.5)
            continue
        if not rep.get("ok"):
            last_err = rep.get("error")
            time.sleep(0.5)
            continue
        info = _decode_result(rep.get("result"))
        if isinstance(info, dict):
            if info.get("pendingOpenPrompt"):
                prompts += 1
                if prompts >= 3:
                    return info, ("the editor is showing its 'Unsaved changes' prompt, which nothing here "
                                  "can answer -- answer it in the editor, or save/discard first")
            elif not info.get("pendingOpen") and not info.get("loading"):
                if _same_path(info.get("level"), want):
                    return info, None
                mismatch += 1
                if mismatch >= 4:        # settled, and not on the level that was asked for
                    return info, ("the editor finished without opening %s (it has %r open) -- see its log "
                                  "for why the load failed" % (want, info.get("level")))
        time.sleep(0.5)
    return info, "the level was still not open after %gs%s" % (timeout, (" (%s)" % last_err) if last_err else "")


def tool_level(args):
    op = args.get("op")
    if op not in _LEVEL_PARAMS:
        return {"ok": False, "error": "op must be one of %s (got %r)" % (", ".join(_LEVEL_PARAMS), op)}
    bad = sorted(k for k in args if k not in _LEVEL_PARAMS[op] and k not in _LEVEL_COMMON)
    if bad:
        return {"ok": False, "op": op,
                "error": "unknown argument(s) %s for op %s -- it takes %s. Refused rather than ignored."
                         % (", ".join(bad), op, ", ".join(sorted(_LEVEL_PARAMS[op])) or "no parameters")}
    try:
        port, source = resolve_editor_port(args)
        timeout = float(args["timeout"]) if "timeout" in args else \
            (_EDITOR_OPEN_TIMEOUT if op == "open" else _EDITOR_CALL_TIMEOUT)
        if not timeout > 0:
            raise ValueError("timeout must be positive")
        fn, abi_args, text = _level_request(op, args)
    except (ValueError, EditorError) as e:
        return {"ok": False, "op": op, "error": str(e)}

    try:
        # `open`'s own request only queues the load, so it never needs the load's budget.
        reply = editor_abi(port, "level", fn, abi_args, text,
                           min(timeout, _EDITOR_CALL_TIMEOUT) if op == "open" else timeout)
    except EditorError as e:
        return {"ok": False, "op": op, "port": port, "error": str(e)}

    out = {"ok": bool(reply.get("ok")), "op": op, "port": port}
    if not out["ok"]:
        err = reply.get("error", "the editor refused with no reason")
        if "no ABI registered for 'level'" in err:
            err += " -- this editor was built before the level ABI existed; rebuild it"
        out["error"] = err
        return out
    value = _decode_result(reply.get("result"))
    if isinstance(value, dict):
        out.update(value)
        # The level parser skips a record it does not know without a word, so the editor lists them; saying
        # so here keeps a misspelt PLCAE from passing as a place that merely placed fewer.
        if op == "place" and value.get("ignored"):
            shown = "; ".join("line %s: %s" % (l.get("line"), l.get("text"))
                              for l in (value.get("ignoredLines") or []) if isinstance(l, dict))
            out["warning"] = ("%s line(s) of the placements were not PLACE/PLACEG/CHILD/CHILDG records and were "
                              "ignored%s" % (value["ignored"], (" -- " + shown) if shown else ""))
    else:
        out["result"] = value
    if op == "open":
        info, why = _wait_for_open(port, out.get("path"), timeout)
        out["info"] = info
        if why:
            out["ok"] = False
            out["error"] = why
        else:
            out["opened"] = True
    return out


TOOLS = [
    {
        "name": "aver_build",
        "description": "Build the engine. Returns whether it built plus any compiler errors, with the "
                       "Ninja progress dropped. Optional Release, build_dir and extra CMake args (e.g. "
                       "-DAVER_MODULE_LANDSCAPE=OFF to check a configuration still builds).",
        "inputSchema": {"type": "object", "properties": {
            "release": {"type": "boolean", "description": "shorthand for config Release"},
            "config": {"type": "string", "enum": list(_BUILD_CONFIGS),
                       "description": "CMAKE_BUILD_TYPE. Reconfigures the tree named by build_dir, "
                                      "so naming the wrong one flips that tree for every later build. "
                                      "Debug builds 'build', Release 'build-release', and the other "
                                      "two their own 'build-<lowercase>' trees."},
            "build_dir": {"type": "string"},
            "cmake_args": {"type": "array", "items": {"type": "string"}},
        }},
        "fn": tool_build,
    },
    {
        "name": "aver_run",
        "description": "Run a host for a fixed number of frames with engine flags, optionally writing "
                       "a screenshot. `host` picks Sandbox.exe (the editor, default) or "
                       "AverEngineRuntime.exe (the shipped game host) -- they do NOT parse the same "
                       "flags, and a flag the chosen host's source never mentions is reported back "
                       "rather than silently ignored. Returns exit code, the parsed probe (raw codes, "
                       "viewport rect, in/outside tag; editor only), errors, warnings and any lines "
                       "matching `grep`. Always terminates: interactive runs are refused. Use "
                       "aver_flags to see what each host takes.",
        "inputSchema": {"type": "object", "properties": {
            "frames": {"type": "integer", "description": "must be > 0; default 40. A bounded run opens "
                                                         "its window WITHOUT focus (Engine.cpp's "
                                                         "`interactive`), so anything that needs "
                                                         "keyboard focus is not being exercised."},
            "host": {"type": "string", "enum": sorted(_HOSTS),
                     "description": "'editor' (Sandbox.exe, default) or 'runtime' "
                                    "(AverEngineRuntime.exe, which takes no render-override flags "
                                    "and no --probe/--probe-rel, and wants a --project or a bare "
                                    ".ocproject/level path to have a world to draw)"},
            "flags": {"type": "array", "items": {"type": "string"}},
            "screenshot": {"type": "string", "description": "path for a PNG of the whole backbuffer"},
            "grep": {"type": "string", "description": "regex; matching log lines are returned"},
            "release": {"type": "boolean"},
            "build_dir": {"type": "string", "description": "build tree to run from (default 'build'). "
                                                           "MUST match the build_dir you passed to "
                                                           "aver_build, or you measure a stale binary "
                                                           "that silently ignores your new flags."},
            "timeout": {"type": "integer"},
        }},
        "fn": tool_run,
    },
    {
        "name": "aver_inspect_image",
        "description": "Crop and/or scale a screenshot so a region is legible. The backbuffer is "
                       "3532x1987, so a toolbar at full frame is a few pixels tall -- every UI defect "
                       "in this tree was found in a crop. box is [x, y, w, h].",
        "inputSchema": {"type": "object", "properties": {
            "path": {"type": "string"},
            "box": {"type": "array", "items": {"type": "integer"}},
            "scale": {"type": "number"},
            "out": {"type": "string"},
        }, "required": ["path"]},
        "fn": tool_inspect_image,
    },
    {
        "name": "aver_tests",
        "description": "Run the headless suites through CTest (./scripts/test.ps1) and report the "
                       "failures. CTest, not the bare .exe files: every *Test target is registered "
                       "with a FAIL_REGULAR_EXPRESSION because suites here have historically exited 0 "
                       "however they went, so a suite that prints `FAIL  ` fails even on exit 0. The "
                       "lines that tripped that pattern come back in `fail_lines`.",
        "inputSchema": {"type": "object", "properties": {
            "only": {"type": "string", "description": "CTest -R name regex (CASE-SENSITIVE, and a "
                                                      "regex -- this used to be a case-insensitive "
                                                      "substring, so 'import' no longer matches "
                                                      "ImportTest; write 'Import')"},
            "release": {"type": "boolean"},
            "build_dir": {"type": "string", "description": "build tree to run from (default 'build'). "
                                                           "MUST match the build_dir you passed to "
                                                           "aver_build, or you run stale executables."},
            "jobs": {"type": "integer", "description": "parallel suites; default is CPU count minus two"},
            "rerun": {"type": "boolean", "description": "only the suites that failed last time"},
            "timeout": {"type": "integer", "description": "seconds for the whole sweep (default 3600); "
                                                          "CTest still applies its own 600s per suite"},
        }},
        "fn": tool_tests,
    },
    {
        "name": "aver_gates",
        "description": "Run the render-gate oracle READ-ONLY for the given configurations and report "
                       "each gate, the invariant checks and the verdict. Cannot re-record: that "
                       "overwrites the only record of what the renderer used to do and is a human's "
                       "call via ./scripts/record-gates.ps1.",
        "inputSchema": {"type": "object", "properties": {
            "configs": {"type": "array", "items": {"type": "string"},
                        "description": "baseline, no-rt, no-ms, sm60, tier1-no-typed-uav, "
                                       "no-cons-raster, all-off, no-dxc, warp"},
            "release": {"type": "boolean"},
        }},
        "fn": tool_gates,
    },
    {
        "name": "aver_package",
        "description": "Stage the ENGINE payload -- what the Aver Launcher pulls and installs -- with "
                       "./scripts/stage-payload.ps1, and optionally prove it runs outside the tree "
                       "that built it with ./scripts/verify-payload.ps1. A payload is the engine, so "
                       "it takes no project. The packaged-GAME path is NOT gone -- it came back with "
                       "AverEngineRuntime.exe, and scripts/stage-game.ps1 plus scripts/verify-game.ps1 "
                       "(whose divergence gate launches both hosts) are how a project is cut; that is "
                       "a human's command, not this tool's. Defaults to Release, because a payload is "
                       "what ships.",
        "inputSchema": {"type": "object", "properties": {
            "out": {"type": "string", "description": "directory to stage the payload into"},
            "build_dir": {"type": "string", "description": "build tree to stage from (default: derived from config)"},
            "config": {"type": "string", "description": "Debug or Release (default Release)"},
            "with_samples": {"type": "boolean", "description": "include the sample content"},
            "force": {"type": "boolean", "description": "replace a non-empty output directory"},
            "verify": {"type": "boolean", "description": "also run verify-payload.ps1 on the result"},
        }, "required": ["out"]},
        "fn": tool_package,
    },
    {
        "name": "aver_flags",
        "description": "List each host's command-line flags, read from that host's own source so the "
                       "list cannot go stale: Sandbox.exe from sandbox/src/Sandbox*, "
                       "AverEngineRuntime.exe from Runtime/. Also reports which flags both take, "
                       "which are editor-only and which are runtime-only, plus the ones this server "
                       "refuses and why.",
        "inputSchema": {"type": "object", "properties": {}},
        "fn": tool_flags,
    },
    {
        "name": "aver_editor",
        "description": "Send ONE raw command to a RUNNING editor's control channel (started with --mcp, or "
                       "Start on the status bar's MCP button) and return the reply. Default cmd is abi: "
                       "{module, fn, args: number[], text: string} calls a registered ABI on the editor's "
                       "main thread -- editor (tool, screenshot, mouse, widgets), physics (bodyCount, "
                       "ready), world (stream_on, stream_off, stream_stats, warp), graph (entity_pos, load, "
                       "attach, tick), level (see aver_level, which is the friendlier way in). Other cmds: "
                       "ping, modules (lists the registered ABIs -- ask this first), widgets, and the input "
                       "commands move/click/key/text/shot, whose own fields go in `extra` (x, y, button, "
                       "widget, key, path). `text` may hold newlines and quotes. The reply's `result` is "
                       "parsed when it is JSON. Refuses with the way to start the channel when nothing is "
                       "listening. The channel is loopback-only and drives the real editor the user may be "
                       "looking at.",
        "inputSchema": {"type": "object", "properties": {
            "cmd": {"type": "string", "enum": list(_EDITOR_CMDS), "description": "default abi"},
            "module": {"type": "string", "description": "abi only: the ABI's registered name"},
            "fn": {"type": "string", "description": "abi only: the entry point, without any module prefix"},
            "args": {"type": "array", "items": {"type": "number"},
                     "description": "abi only: numeric arguments in order (a flag is 0 or 1)"},
            "text": {"type": "string", "description": "the one string argument: an abi's text, or the "
                                                      "typed text of cmd=text"},
            "extra": {"type": "object", "description": "non-abi commands: their own fields, e.g. "
                                                       "{\"x\":10,\"y\":20,\"button\":\"left\"} for click"},
            "port": {"type": "integer", "description": "the editor's port; default mcp.conf's "
                                                       "editor_bridge.port, else 45123"},
            "timeout": {"type": "number", "description": "seconds to wait for the answer; default 90"},
        }},
        "fn": tool_editor,
    },
    {
        "name": "aver_level",
        "description": "Assemble and edit the level a RUNNING editor has open (started with --mcp, or Start on "
                       "the status bar's MCP button), through the editor's own code paths: every edit is "
                       "undoable with Ctrl+Z where the UI's is, dirties the level, and saves exactly as a click "
                       "would. `op` picks the operation. Ids are entity ids (info and list report them). "
                       "Positions are cm, Z up; rotations are degrees [yaw, pitch, roll]; transforms are WORLD "
                       "space. Refused, with the reason, while the editor is playing, with no level open, while "
                       "a level open is pending, and on an unknown or foreign entity id.\n"
                       "OPS (parameters -> reply fields):\n"
                       "open {path, discard?} -> {opened, path, info}. Level file relative to the project's "
                       "Content dir (Maps/Arena.ocworld, or Arena) or absolute. Waits for the load. Refused "
                       "while the open level has unsaved edits unless discard:true throws them away "
                       "(discard must be a real boolean, or 0/1: anything else is refused, never guessed).\n"
                       "info {} -> {level, name, entities, dirty, format, playing, loading, pendingOpen, "
                       "pendingOpenPrompt, content, playerStart:{id,pos,yaw}|null, camera:{pos,yaw,pitch}, "
                       "selection:[ids]}\n"
                       "list {filter?, max?} -> {total, returned, entities:[{id, asset, label, pos:[x,y,z], "
                       "rot:[yaw,pitch,roll], scale:[x,y,z], material, collide, visible, parent, "
                       "anim:{clip,speed,time,once}|null}]}. filter is a case-insensitive substring of asset "
                       "or label; max defaults to 500. Works while playing (poses are the live ones).\n"
                       "place {placements} -> {ids, placed, requested, parsed, ignored, ignoredLines, animated, "
                       "undoEntries, warning?}. placements is "
                       "one string of .ocworld lines (newline-separated) or an array of lines, parsed by the "
                       "real level parser, so every token is honoured: PLACE <asset> x y z yaw pitch roll "
                       "<scale> [material] [nocollide] [hidden] [snap] [name <percent-encoded>] [anim "
                       "<clip.ocanim> animspeed <f> animtime <f> animonce] [vehicle car|van|truck|bus|sports: a "
                       "car that DRIVES in Play on the level's <name>.oclanes, and gets no static collider]; "
                       "PLACEG takes sx sy sz instead of "
                       "<scale>; CHILD/CHILDG lines inside BEGIN/END parent to the line above. asset is a "
                       "content-relative .ocmesh path with forward slashes (Meshes/cube.ocmesh); it must "
                       "exist. ids is one per placement, 0 where the world refused. requested counts the "
                       "record lines sent, parsed the placements read from them; any other line (a misspelt "
                       "keyword) is skipped and listed in ignoredLines, with a warning. Refused, nothing "
                       "placed: class placements; a NaN, infinite or out-of-range number; an anim clip that "
                       "is not an object clip in the content index, or on a legacy .ocmap level (its save "
                       "cannot store one). One undo entry per entity (the editor keeps the last 128).\n"
                       "set_transform {id, position, rotation, scale?} -> {id, pos, rot, scale}. Undoable. "
                       "Refused while a drag is in flight in the editor, and for a `snap` placement (a save "
                       "keeps its authored ground offset, so the move would be lost: remove and re-place it).\n"
                       "set_material {id, material} -> {id, material}. material is an .ocmat stem (M_Wood). "
                       "Not undoable, like the Details panel's picker.\n"
                       "set_collide {id, collide} -> {id, collide, changed}. Undoable. "
                       "set_visible {id, visible} -> {id, visible, changed}. Undoable; saved with the level.\n"
                       "set_anim {id, clip, speed?, time?, once?} -> {id, anim, changed}. clip is a "
                       "content-relative .ocanim OBJECT clip in the content index (\"\" clears); speed default "
                       "1, time = the start time in seconds, once = play once and hold. Undoable. Refused on "
                       "a legacy .ocmap level. The Player Start marker takes only set_transform, select, "
                       "remove and player_start.\n"
                       "remove {ids} -> {removed, requested}. One undo entry per entity.\n"
                       "select {ids, frame?} -> {selected, framed}. Selects and frames the camera on them "
                       "(frame:false to leave the camera); empty ids clears the selection.\n"
                       "save {path?} -> {path, saved, savedAs}. No path saves in place. A bare name saves "
                       "as Content/Maps/<name>.ocworld; a path (relative to Content, or absolute) is Save As.\n"
                       "player_start {position, yaw?} -> {id, pos, yaw, created}. Moves the level's Player "
                       "Start, or creates it (one per level).",
        "inputSchema": {"type": "object", "properties": {
            "op": {"type": "string", "enum": list(_LEVEL_PARAMS)},
            "path": {"type": "string", "description": "open, save: the level file (see the op)"},
            "discard": {"type": "boolean", "description": "open: throw away the open level's unsaved edits"},
            "filter": {"type": "string", "description": "list: case-insensitive substring of asset or label"},
            "max": {"type": "integer", "description": "list: most rows to return; default 500"},
            "placements": {"anyOf": [{"type": "string"}, {"type": "array", "items": {"type": "string"}}],
                           "description": "place: .ocworld PLACE/PLACEG/CHILD lines"},
            "id": {"type": "integer", "description": "the entity: set_transform, set_material, set_collide, "
                                                     "set_visible, set_anim"},
            "ids": {"type": "array", "items": {"type": "integer"}, "description": "remove, select"},
            "position": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3,
                         "description": "[x, y, z] cm: set_transform, player_start"},
            "rotation": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3,
                         "description": "[yaw, pitch, roll] degrees: set_transform"},
            "scale": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3,
                      "description": "[sx, sy, sz]: set_transform; omitted keeps the current scale"},
            "yaw": {"type": "number", "description": "player_start: heading in degrees; default 0"},
            "material": {"type": "string", "description": "set_material: an .ocmat stem"},
            "collide": {"type": "boolean", "description": "set_collide"},
            "visible": {"type": "boolean", "description": "set_visible"},
            "clip": {"type": "string", "description": "set_anim: content-relative .ocanim path; \"\" clears"},
            "speed": {"type": "number", "description": "set_anim: playback rate; default 1"},
            "time": {"type": "number", "description": "set_anim: start time in seconds; default 0"},
            "once": {"type": "boolean", "description": "set_anim: play once and hold the end"},
            "frame": {"type": "boolean", "description": "select: frame the camera on the selection; default true"},
            "port": {"type": "integer", "description": "the editor's port; default mcp.conf's "
                                                       "editor_bridge.port, else 45123"},
            "timeout": {"type": "number", "description": "seconds to wait; default 90 (300 for open, which "
                                                         "waits for the load)"},
        }, "required": ["op"]},
        "fn": tool_level,
    },
]

BY_NAME = {t["name"]: t for t in TOOLS}


# --------------------------------------------------------------------------------------------------
# JSON-RPC 2.0 over newline-delimited stdio
# --------------------------------------------------------------------------------------------------

def handle(msg):
    method = msg.get("method")
    mid = msg.get("id")

    if method == "initialize":
        # The client's protocol version is echoed back rather than pinned to one this file knows about,
        # so a newer client is not refused by a server whose only job is to run a build.
        want = (msg.get("params") or {}).get("protocolVersion", "2025-06-18")
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": want,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        }}

    if method in ("notifications/initialized", "initialized"):
        return None                       # a notification: no id, no reply

    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "tools": [{k: t[k] for k in ("name", "description", "inputSchema")} for t in TOOLS]}}

    if method == "tools/call":
        params = msg.get("params") or {}
        name = params.get("name")
        tool = BY_NAME.get(name)
        if not tool:
            return {"jsonrpc": "2.0", "id": mid,
                    "error": {"code": -32601, "message": "unknown tool %r" % name}}
        try:
            result = tool["fn"](params.get("arguments") or {})
        except Exception as e:
            # Reported as a tool RESULT rather than a protocol error: a build that blew up is an answer
            # about the build, and a caller should see it as one instead of as a broken server.
            result = {"ok": False, "error": "%s: %s" % (type(e).__name__, e)}
        # WHICH CHECKOUT THIS PROCESS ACTED IN, on every result, unconditionally -- not just tool_run's
        # existing binary_provenance (which reports a path RELATIVE to ROOT, so it never actually reveals
        # which ROOT that was). One long-lived process has exactly one ROOT for its entire lifetime, and
        # that ROOT is fixed by wherever the *.mcp.json (or whoever else) pointed the interpreter at this
        # script -- NOT by the caller's own cwd or intent. A stale or hand-edited launcher can point at a
        # different checkout than the one the caller is scoped to, and until now nothing in the response
        # said so: the wrong tree answered silently (see the long comment above resolve_tree, and
        # memory/aver-mcp-tools-wrong-tree.md). This does not choose or fix which tree gets used -- it
        # makes whichever one was used impossible to miss.
        if isinstance(result, dict):
            result.setdefault("root", ROOT)
        payload = json.dumps(result, indent=2)
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "content": [{"type": "text", "text": payload}],
            "isError": not result.get("ok", True),
        }}

    if mid is None:
        return None                       # any other notification
    return {"jsonrpc": "2.0", "id": mid,
            "error": {"code": -32601, "message": "unsupported method %r" % method}}


def serve_stdio():
    """The original, and still default, transport. Unchanged: a caller on stdio today gets exactly
    what it got before this file grew an HTTP option."""
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError as e:
            log("bad JSON: %s" % e)
            continue
        reply = handle(msg)
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()


# Loopback ONLY -- matches modules/mcp's McpBridge, which binds 127.0.0.1 for the same reason (this is
# a build/test/package runner with no auth of its own; reachable-from-the-network is a very different
# risk profile and nothing in this task asked for it). Not configurable by design: the port is the only
# knob mcp.conf exposes for this server, on purpose.
_HTTP_HOST = "127.0.0.1"
_HTTP_PATH = "/mcp"


class _McpHTTPHandler(http.server.BaseHTTPRequestHandler):
    """POST /mcp with a single JSON-RPC message; get back its reply. That's the whole surface.

    Maps handle(msg)'s existing return convention onto HTTP the only way that's consistent with it:
    a dict reply -> 200 application/json with that dict as the body; None (a notification, e.g.
    notifications/initialized) -> 202 with an empty body, which is what a real client (Claude Code)
    was proven to send and accept -- see the protocol transcripts in the Part 2 report. GET is refused
    with 405: the SSE-push half of Streamable HTTP is not implemented because the live capture showed
    a client that never needed a server-initiated push tolerates a 405 on that GET without incident.
    """

    server_version = "aver-mcp-http/1"

    def log_message(self, fmt, *args):
        log("http %s - %s" % (self.address_string(), (fmt % args)))

    def _reply_json(self, status, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _reply_empty(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        if self.path.split("?", 1)[0].rstrip("/") != _HTTP_PATH:
            self._reply_json(404, {"jsonrpc": "2.0", "id": None,
                                    "error": {"code": -32600, "message": "no such endpoint %r; use %s"
                                              % (self.path, _HTTP_PATH)}})
            return
        try:
            length = int(self.headers.get("Content-Length", "0") or "0")
        except ValueError:
            length = 0
        raw = self.rfile.read(length) if length > 0 else b""
        try:
            msg = json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            self._reply_json(400, {"jsonrpc": "2.0", "id": None,
                                    "error": {"code": -32700, "message": "parse error: %s" % e}})
            return
        try:
            reply = handle(msg)
        except Exception as e:
            self._reply_json(500, {"jsonrpc": "2.0", "id": msg.get("id"),
                                    "error": {"code": -32603, "message": "%s: %s" % (type(e).__name__, e)}})
            return
        if reply is None:
            self._reply_empty(202)          # a notification: acknowledged, nothing to say back
        else:
            self._reply_json(200, reply)

    def do_GET(self):
        self._reply_empty(405)              # server-initiated push not implemented; see class docstring


class _McpHTTPServer(http.server.ThreadingHTTPServer):
    """ThreadingHTTPServer that REFUSES an already-occupied port instead of quietly sharing it.

    allow_reuse_address defaults to True in socketserver, and on Windows that flag does not mean
    what it means on Unix: SO_REUSEADDR there permits a genuine second bind to a port another
    socket is already LISTENING on, rather than only reclaiming a TIME_WAIT one. So the default
    lets a second `aver_mcp.py --http --port N` start "successfully" beside a first -- two live
    servers, each possibly rooted at a DIFFERENT checkout, with the OS deciding which one a given
    connection reaches. That is precisely the wrong-tree confusion this whole change exists to
    remove, arrived at from the other end: not a build reported against the wrong root, but a
    client silently talking to the wrong server.

    Refusing costs nothing real. The port only matters while a server holds it, and a stale
    TIME_WAIT on a loopback listener clears in seconds; an operator who genuinely wants the port
    back should stop the process holding it, not race it.
    """
    allow_reuse_address = False


def serve_http(port):
    try:
        httpd = _McpHTTPServer((_HTTP_HOST, port), _McpHTTPHandler)
    except OSError as e:
        # A BOUND PORT IS AN ENVIRONMENT FAULT, NOT A CRASH. Unhandled, this surfaced as a raw
        # WinError 10013 traceback and exit 1, which reads as a broken server rather than as
        # "something is already there". The editor's own control channel already answers this
        # situation by logging why and carrying on (McpBridge::start returns false); a tool server
        # has nothing to carry on WITH, so it exits -- but it exits saying what is wrong and how to
        # find the process holding the port.
        log("cannot serve HTTP on %s:%d -- %s" % (_HTTP_HOST, port, e))
        log("something is probably already listening there. Find it with: "
            "netstat -ano | findstr :%d   (then choose another port with --port, or in mcp.conf's "
            "tool_server.port)" % port)
        return 3   # ExitCode.Environment (core/ErrorCodes.hpp)
    log("serving from %s over http://%s:%d%s (Streamable HTTP; POST only, GET replies 405)"
        % (ROOT, _HTTP_HOST, port, _HTTP_PATH))
    rc = 0
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        rc = 4   # ExitCode.Interrupted
    finally:
        httpd.server_close()
    return rc


def main():
    argv = sys.argv[1:]
    port, source = resolve_tool_server_port(argv)
    if "--http" in argv:
        return serve_http(port)
    # Default path, exactly as before --http existed: stdio, no socket opened, nothing about this
    # process's behaviour on .mcp.json's existing stdio launch has changed.
    log("serving from %s over stdio (tool_server.port=%d, source=%s -- pass --http to serve over "
        "HTTP on that port instead)" % (ROOT, port, source))
    serve_stdio()
    return 0


if __name__ == "__main__":
    sys.exit(main())
