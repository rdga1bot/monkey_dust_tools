#!/usr/bin/env python3
"""
game_cmd_driver.py — reusable live-command driver for an already-running
monkey_dust game process, via tmp_/game_cmd/ (game/src/ui_driver/
game_cmd_file.cpp).

Promoted from a session-local script (originally
tmp_/perf_master/game_cmd_driver.py, written for a perf-measurement task)
into permanent tools/qa/ infrastructure — 2026-08-15 session found this is
the reliable way to self-verify visual terrain/shader bugs (screenshot,
camera positioning) WITHOUT depending on the editor's `--exec` scenario
mode, which reuses the interactive render loop and can silently fail to
ever produce a composited frame if the window is hidden/not yet mapped
(see CLAUDE_CONSTITUTION.md ADR entry for the real root cause found this
session: editor main() never called SDL_ShowWindow()). A normally-launched
game process (no --exec) renders every frame for real regardless of
pending commands, so driving it live via this file-based channel avoids
that whole class of bug.

Protocol (game/src/ui_driver/game_cmd_file.cpp): write cmd.lua, bump
cmd.seq, poll result.json for a matching "seq" field. One Lua chunk per
send() -- no return values cross the file boundary, only ok/error.
get_number() round-trips a value via md.log() + stdout (game Lua has no
`io` global, sandboxed) -- redirect the process's stdout to a file and
tail it after each send().

Requires a MONKEY_DUST_EDITOR=ON build (md.set_camera_pose/md.screenshot/
md.set_editor_open are only registered in that config -- see
game/src/scripting/lua_scenario_api_misc.cpp's #ifdef guards). `build/`
in this repo is such a build by default (CMakeLists.txt's
MONKEY_DUST_EDITOR option defaults ON).

USAGE (as a library):
    from game_cmd_driver import Driver
    d = Driver(exe="build/game/monkey_dust")
    d.launch()
    d.send("md.set_camera_pose(10148.7, 40.0, 15657.6, 26.6, 22.0)")
    d.send("md.set_editor_open(false)")   # set_camera_pose forces this
                                            # true as a side effect --
                                            # call AFTER, not before
    d.send("md.screenshot('/tmp/out.png')")
    d.shutdown()

USAGE (CLI, the exact 4-call sequence above in one shot):
    python3 tools/qa/game_cmd_driver.py --screenshot /tmp/out.png \\
        --camera 10148.7,40.0,15657.6,26.6,22.0
    python3 tools/qa/game_cmd_driver.py --screenshot /tmp/out.png \\
        --camera 10148.7,40.0,15657.6,26.6,22.0 \\
        --exe build_release/game/monkey_dust --wait 1.5
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent.parent
DEFAULT_EXE = str(_REPO / "build" / "game" / "monkey_dust")
CMD_DIR = _REPO / "tmp_" / "game_cmd"
SEQ_PATH = CMD_DIR / "cmd.seq"
CMD_PATH = CMD_DIR / "cmd.lua"
RESULT_PATH = CMD_DIR / "result.json"
STDOUT_PATH = CMD_DIR / "game_stdout.log"


class Driver:
    def __init__(self, exe=DEFAULT_EXE, cwd=None):
        self.exe = exe
        self.cwd = cwd or str(_REPO)
        self.proc = None
        self._seq = 0
        self._val_counter = 0
        self._stdout_f = None

    def launch(self, wait_s=25, argv=None, env=None):
        # Stale files from a previous run could let the first send() match
        # an old seq by accident -- start clean (same reasoning as
        # tools/editor/editor_cmd_file.cpp's analogous channel).
        os.makedirs(CMD_DIR, exist_ok=True)
        for p in (SEQ_PATH, RESULT_PATH, STDOUT_PATH):
            if p.exists():
                p.unlink()
        self._stdout_f = open(STDOUT_PATH, "wb")
        popen_env = {**os.environ, **env} if env else None
        self.proc = subprocess.Popen(argv or [self.exe], cwd=self.cwd, env=popen_env,
                                      stdout=self._stdout_f, stderr=subprocess.STDOUT)
        deadline = time.time() + wait_s
        while time.time() < deadline:
            ok, _ = self.send("md.log('driver: connected')", timeout=1.0)
            if ok:
                return True
            time.sleep(0.3)
        return False

    def send(self, lua_code, timeout=5.0):
        self._seq += 1
        with open(CMD_PATH, "w") as f:
            f.write(lua_code)
        with open(SEQ_PATH, "w") as f:
            f.write(str(self._seq))
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with open(RESULT_PATH) as f:
                    content = f.read()
            except FileNotFoundError:
                time.sleep(0.02)
                continue
            m = re.search(r'"seq":(\d+)', content)
            if m and int(m.group(1)) == self._seq:
                return ('"ok":true' in content), content
            time.sleep(0.02)
        return False, "TIMEOUT waiting for seq=%d" % self._seq

    def get_number(self, expr):
        self._val_counter += 1
        marker = "MDVAL:%d:" % self._val_counter
        lua = "md.log('%s' .. tostring(%s))" % (marker, expr)
        ok, content = self.send(lua)
        if not ok:
            return None, content
        try:
            with open(STDOUT_PATH, "r", errors="replace") as f:
                text = f.read()
        except FileNotFoundError:
            return None, "stdout log missing"
        idx = text.rfind(marker)
        if idx < 0:
            return None, "marker not found: %s" % marker
        line_end = text.find("\n", idx)
        val_str = text[idx + len(marker): line_end if line_end >= 0 else None].strip()
        try:
            return float(val_str), None
        except ValueError as e:
            return None, "%s (raw=%r)" % (e, val_str)

    def screenshot(self, out_path, camera=None, editor_open=False, settle_s=1.0):
        """camera: (x, y, z, yaw_deg, pitch_deg) or None to leave as-is.
        editor_open=False clears the F3 panel (set_camera_pose forces it
        true as a side effect, so this is applied AFTER positioning, same
        order bug found live this session -- see module doc)."""
        if camera is not None:
            ok, r = self.send("md.set_camera_pose(%s)" % ", ".join(str(c) for c in camera))
            if not ok:
                return False, r
        ok, r = self.send("md.set_editor_open(%s)" % ("true" if editor_open else "false"))
        if not ok:
            return False, r
        time.sleep(settle_s)
        return self.send("md.screenshot(%r)" % str(out_path))

    def shutdown(self):
        if self.proc:
            self.send("md.quit(0)", timeout=1.0)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None
        if self._stdout_f:
            self._stdout_f.close()
            self._stdout_f = None


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0.0
    if n % 2 == 1:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2.0


def stdev(xs, m):
    if len(xs) < 2:
        return 0.0
    return (sum((x - m) ** 2 for x in xs) / (len(xs) - 1)) ** 0.5


def _main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=DEFAULT_EXE, help="game executable path")
    ap.add_argument("--screenshot", metavar="PATH", help="capture a screenshot to PATH")
    ap.add_argument("--camera", metavar="x,y,z,yaw,pitch",
                     help="position camera before capture (comma-separated floats)")
    ap.add_argument("--wait", type=float, default=1.0,
                     help="seconds to settle after camera move before capture (default 1.0)")
    args = ap.parse_args()

    if not args.screenshot:
        print("Nothing to do -- pass --screenshot PATH (see module docstring for library usage)",
              file=sys.stderr)
        return 1

    camera = None
    if args.camera:
        parts = [float(x) for x in args.camera.split(",")]
        if len(parts) != 5:
            print("ERROR: --camera needs 5 comma-separated values: x,y,z,yaw,pitch",
                  file=sys.stderr)
            return 1
        camera = tuple(parts)

    d = Driver(exe=args.exe)
    print(f"launching {args.exe} ...")
    if not d.launch():
        print("FAILED to connect", file=sys.stderr)
        return 1
    print("connected")
    ok, r = d.screenshot(args.screenshot, camera=camera, settle_s=args.wait)
    print("screenshot:", ok, r)
    d.shutdown()
    if not ok:
        return 1
    if not Path(args.screenshot).exists():
        print(f"ERROR: {args.screenshot} was not created despite ok=true", file=sys.stderr)
        return 1
    print(f"done -> {args.screenshot}")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
