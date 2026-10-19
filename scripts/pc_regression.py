"""Automated regression run for pc-compat games: scripted, hidden, muted sessions checked from positive evidence.

Each game describes its scenarios as data in games/<SERIAL>/profile.json, pcCompat.regression (see that file). A run
copies the freshly built layer (build/pc-compat/d3d8.dll) and the profile's spartacompat.ini into a test install,
plays every scenario with scripted keys, and checks what each one must show:

  * the session itself: the layer loaded, frames were presented, the run lasted until its timer (no crash or hang);
  * log lines that must appear (patches applied, a save written, ...) and lines that must not (refused patches);
  * frame dumps: not blank (one colour >= 99.5 %% or <= 2 colours is blank), optionally the hor+ side areas drawn;
  * texture coverage (images without a replacement) and files the scenario must leave (a save of the right size);
  * settings the in-game overlay must have saved (expectIni);
  * ini: {"Section.Key": value} settings for one scenario (the file is restored afterwards);
  * closeAfterSeconds: the game window gets WM_CLOSE (what Alt+F4 sends) at that time; the session must then end by
    itself with exit code 0 instead of running until its timer. spartacompat.ini is put back after every scenario, so a
    scenario that changes settings through the overlay never affects the next one.

A check passes only from evidence of what it claims; a run that cannot show it reports "unknown", never "pass"
(lesson pipeline.completion.absence-of-errors-is-not-a-session). Frames and logs are derived from the user's game:
results stay under work/ (default <test install>/../regression/<UTC time>/), never in the repository.

usage:
  python scripts/pc_regression.py --serial PC-DPTWP-UK --game <test copy of the game> [--only save reload]
                                  [--dll build/pc-compat/d3d8.dll] [--keep-ini] [--out <dir>]
Exit code 0 = every check passed, 1 = a check failed or could not be decided, 2 = bad arguments.
Requires Pillow (frame checks).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_jsonc(path):
    """JSON with // comments (game profiles), comments outside strings only."""
    with open(path, encoding="utf-8") as fh:
        s = fh.read()
    out, i = [], 0
    in_str = esc = False
    while i < len(s):
        c = s[i]
        if in_str:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            out.append(c)
        elif s.startswith("//", i):
            while i < len(s) and s[i] != "\n":
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    return json.loads("".join(out))


# ---------------------------------------------------------------- frame checks (pure, unit-tested)

def blank_fraction(colors, total):
    """Share of the most common colour; colors = Image.getcolors() list of (count, colour)."""
    if not colors or not total:
        return 1.0
    return max(c for c, _ in colors) / total


def is_blank(img):
    """One colour on >= 99.5 % of the picture, or at most two colours: nothing was really drawn."""
    small = img.convert("RGB").resize((max(1, img.width // 4), max(1, img.height // 4)), 0)  # nearest: no new colours
    colors = small.getcolors(1 << 16)
    total = small.width * small.height
    if colors is None:  # more than 65536 colours: certainly drawn
        return False
    return len(colors) <= 2 or blank_fraction(colors, total) >= 0.995


def side_black_share(img, centre_fraction):
    """Share of pure-black pixels in the hor+ side strips (outside the centred 4:3 part)."""
    g = img.convert("L")
    w, h = g.size
    x0, x1 = int(w * (1 - centre_fraction) / 2), int(w * (1 + centre_fraction) / 2)
    px = g.load()
    black = n = 0
    for y in range(0, h, 4):
        for x in list(range(0, x0, 4)) + list(range(x1, w, 4)):
            n += 1
            black += px[x, y] < 3
    return black / n if n else 0.0


# ---------------------------------------------------------------- log checks (pure, unit-tested)

def count_stalls(text, movie_window=30.0):
    """'No new frame for 10 s' reports that are real stalls: not before the first frame (start-up, intro movies)
    and not within movie_window seconds after a movie started (movies play outside the game's frame loop)."""
    stalls, last_movie = 0, -1e9
    for line in text.splitlines():
        m = re.match(r"\[\s*([\d.]+)\] (.*)", line)
        if not m:
            continue
        t, msg = float(m.group(1)), m.group(2)
        if msg.startswith("movie: video window") or msg.startswith("game: Playing STR"):
            last_movie = t
        s = re.match(r"\[state\] no new frame for 10 s \((\d+) presented\)", msg)
        if s and int(s.group(1)) > 0 and t - last_movie > movie_window:
            stalls += 1
    return stalls


def parse_log(text):
    """Facts a scenario can be judged on, from spartacompat.log."""
    facts = {"loaded": "compatibility layer loaded" in text,
             "ended_by_timer": "SPARTA_EXIT_AFTER_MS reached" in text,
             "presented": 0, "missing_textures": None, "exceptions": len(re.findall(r"^\[\s*[\d.]+\] exception ", text, re.M)),
             "stalls": count_stalls(text)}
    for m in re.finditer(r"\[state\] \d+ frames in the last 10 s \([\d.]+ fps\), (\d+) presented", text):
        facts["presented"] = max(facts["presented"], int(m.group(1)))
    for m in re.finditer(r"\[textures\] .*?distinct images \((\d+) without a replacement\)", text):
        facts["missing_textures"] = int(m.group(1))
    return facts


def check_log(text, expect, forbid, game=None):
    """[(name, status, detail)] for regexes that must / must not match a log line. An expected entry may be
    {"rx": ..., "needs": "<path in the test copy>"}: skipped (not failed) when that optional part is absent, e.g. the
    HD texture pack in a copy fresh from the setup."""
    res = []
    for e in expect:
        rx = e["rx"] if isinstance(e, dict) else e
        need = e.get("needs") if isinstance(e, dict) else None
        if need and game and not os.path.exists(os.path.join(game, need)):
            res.append(("log has /%s/" % rx, "skip", "this copy has no %s" % need))
            continue
        m = re.search(rx, text, re.M)
        res.append(("log has /%s/" % rx, "pass" if m else "fail", m.group(0)[:120] if m else "not found"))
    for rx in forbid:
        m = re.search(rx, text, re.M)
        res.append(("log lacks /%s/" % rx, "fail" if m else "pass", m.group(0)[:120] if m else "absent"))
    return res


def read_ini_value(path, section, key):
    """Value of key in [section] of a Windows ini file (names case-insensitive, as the layer reads them), None if absent."""
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return None
    sec = None
    for line in lines:
        t = line.strip()
        if not t or t.startswith(";"):
            continue
        if t.startswith("[") and t.endswith("]"):
            sec = t[1:-1].strip().lower()
        elif sec == section.lower() and "=" in t:
            k, v = t.split("=", 1)
            if k.strip().lower() == key.lower():
                return v.strip()
    return None


def check_ini(path, expect):
    """[(name, status, detail)] for {"key": "Section.Key", "value": ...} entries the session must have saved."""
    res = []
    for e in expect:
        sec, key = e["key"].split(".", 1)
        v = read_ini_value(path, sec, key)
        res.append(("ini %s = %s" % (e["key"], e["value"]), "pass" if v == str(e["value"]) else "fail",
                    "missing" if v is None else v))
    return res


def close_game_windows(pid):
    """Post WM_CLOSE (what Alt+F4 sends) to the visible-or-hidden top-level windows of a process; returns how many."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []

    def cb(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and not user32.GetParent(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)(cb), 0)
    for h in found:
        user32.PostMessageW(h, 0x0010, 0, 0)
    return len(found)


# ---------------------------------------------------------------- running a scenario

def run_scenario(sc, game, exe, out_dir):
    sdir = os.path.join(out_dir, sc["name"])
    frames = os.path.join(sdir, "frames")
    os.makedirs(frames, exist_ok=True)
    for f in sc.get("deleteFiles", []):
        p = os.path.join(game, f)
        if os.path.isfile(p):
            os.remove(p)
    before = []  # commands run first, e.g. the save editor on the save an earlier scenario wrote
    for cmd in sc.get("before", []):
        argv = [sys.executable if c == "python" else c.replace("{game}", game).replace("{repo}", REPO) for c in cmd]
        r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
        before.append(("before: %s" % " ".join(cmd[:3]), "pass" if r.returncode == 0 else "fail",
                       (r.stdout + r.stderr).strip().splitlines()[-1][:120] if (r.stdout + r.stderr).strip() else "exit %d" % r.returncode))
    env = dict(os.environ)
    env.update({"SPARTA_WINDOW_HIDDEN": "1", "SPARTA_MUTE": "1", "SPARTA_EXIT_AFTER_MS": str(int(sc["seconds"] * 1000)),
                "SPARTA_FRAME_DUMP_DIR": frames, "SPARTA_FRAME_DUMP_INTERVAL_MS": str(sc.get("dumpMs", 3000)),
                "SPARTA_FRAME_DUMP_FROM_MS": str(sc.get("dumpFromMs", 0)), "SPARTA_INPUT_SCRIPT": sc.get("keys", ""),
                "SPARTA_FRAME_DUMP_FULL": "1" if sc.get("fullFrames") else "", "SPARTA_TEX_DUMP_DIR": "",
                "SPARTA_FPS_LIMIT": ""})
    env.update({k: str(v) for k, v in sc.get("env", {}).items()})
    for k in [k for k, v in env.items() if k.startswith("SPARTA_") and v == ""]:
        del env[k]  # an empty variable still exists for the layer (it would switch full-size dumps on)
    ini = os.path.join(game, "spartacompat.ini")
    ini_before = open(ini, "rb").read() if os.path.isfile(ini) else None
    if sc.get("ini") and os.name == "nt":  # {"Section.Key": "value"}: settings for this scenario only (restored after)
        import ctypes
        for k, v in sc["ini"].items():
            sec, key = k.split(".", 1)
            ctypes.windll.kernel32.WritePrivateProfileStringW(sec, key, str(v), ini)
    start = time.time()
    proc = subprocess.Popen([exe], cwd=game, env=env)
    closed = None
    if sc.get("closeAfterSeconds"):
        try:
            proc.wait(timeout=sc["closeAfterSeconds"])
        except subprocess.TimeoutExpired:
            closed = close_game_windows(proc.pid)
    try:
        code = proc.wait(timeout=sc["seconds"] + 45)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = "killed after %d s" % (sc["seconds"] + 45)
    elapsed = time.time() - start
    log_path = os.path.join(game, "spartacompat.log")
    text = open(log_path, encoding="utf-8", errors="replace").read() if os.path.exists(log_path) else ""
    if text:
        shutil.copy(log_path, os.path.join(sdir, "spartacompat.log"))
    r = judge(sc, game, text, frames, code, elapsed, closed)
    r["checks"] = [{"check": c, "status": st, "detail": d} for c, st, d in before] + r["checks"]
    if ini_before is not None:
        changed = open(ini, "rb").read() != ini_before
        if changed and not sc.get("expectIni") and not sc.get("ini"):
            r["checks"].append({"check": "settings file unchanged", "status": "warn",
                                "detail": "the session rewrote spartacompat.ini (restored)"})
        open(ini, "wb").write(ini_before)  # the next scenario starts from the same settings
    return r


def judge(sc, game, text, frames, code, elapsed, closed=None):
    facts = parse_log(text)
    res = [("layer loaded", "pass" if facts["loaded"] else "fail", "log present" if text else "no log"),
           ("frames presented", "pass" if facts["presented"] > 0 else "fail", "%d presented" % facts["presented"]),
           (("closed like Alt+F4, exit 0", "pass" if closed and code == 0 and not facts["ended_by_timer"] else "fail",
             "%s window(s) closed, exit %s after %.0f s" % (closed, code, elapsed)) if sc.get("closeAfterSeconds") else
            ("ran until its timer", "pass" if facts["ended_by_timer"] else "fail", "exit %s after %.0f s" % (code, elapsed))),
           ("no 10 s stall", "pass" if facts["stalls"] == 0 else "fail", "%d stall report(s)" % facts["stalls"])]
    if facts["exceptions"]:
        res.append(("first-chance exceptions", "warn", "%d logged (the game may handle them)" % facts["exceptions"]))
    res += check_log(text, sc.get("expectLog", []), sc.get("forbidLog", []), game)
    res += check_ini(os.path.join(game, "spartacompat.ini"), sc.get("expectIni", []))
    pack = sc.get("texturePackFolder", "textures")
    if "maxMissingTextures" in sc and not os.path.isdir(os.path.join(game, pack)):
        res.append(("textures without a replacement <= %d" % sc["maxMissingTextures"], "skip", "this copy has no %s" % pack))
    elif "maxMissingTextures" in sc:
        m = facts["missing_textures"]
        res.append(("textures without a replacement <= %d" % sc["maxMissingTextures"],
                    "unknown" if m is None else ("pass" if m <= sc["maxMissingTextures"] else "fail"),
                    "no texture statistics" if m is None else "%d" % m))
    for f in sc.get("expectFiles", []):
        p = os.path.join(game, f["path"])
        ok = os.path.isfile(p) and ("size" not in f or os.path.getsize(p) == f["size"])
        res.append(("file %s" % f["path"], "pass" if ok else "fail",
                    "%d bytes" % os.path.getsize(p) if os.path.isfile(p) else "missing"))
    res += check_frames(sc, frames)
    return {"name": sc["name"], "facts": facts, "checks": [{"check": c, "status": s, "detail": d} for c, s, d in res]}


def check_frames(sc, frames):
    from PIL import Image
    names = sorted(f for f in os.listdir(frames) if f.endswith(".bmp")) if os.path.isdir(frames) else []
    if not names:
        return [("frame dumps", "unknown", "no frame was dumped")]
    res = []
    images = {}
    for f in names:  # the exit timer can stop the game while it writes a dump: such a file is skipped
        try:
            im = Image.open(os.path.join(frames, f))
            im.load()
            images[f] = im
        except OSError:
            pass
    names = sorted(images)
    blank = [f for f in names if is_blank(images[f])]
    need = sc.get("minDrawnFrames", 1)
    drawn = len(names) - len(blank)
    res.append(("drawn (non-blank) frames >= %d" % need, "pass" if drawn >= need else "fail",
                "%d of %d readable drawn" % (drawn, len(names))))
    side = sc.get("sideBlack")  # {"fromMs": .., "toMs": .., "max": .., "centre": 0.558}
    if side:
        shares = []
        for f in names:
            m = re.match(r"frame_(\d+)_ms\.bmp$", f)
            if m and side.get("fromMs", 0) <= int(m.group(1)) <= side.get("toMs", 10 ** 9) and f not in blank:
                shares.append(side_black_share(images[f], side.get("centre", 1920 / 3440)))
        if not shares:
            res.append(("hor+ side areas drawn", "unknown", "no drawn frame in the window"))
        else:
            mean = sum(shares) / len(shares)
            res.append(("hor+ side areas: black share <= %.2f" % side["max"], "pass" if mean <= side["max"] else "fail",
                        "mean %.3f over %d frame(s)" % (mean, len(shares))))
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", required=True)
    ap.add_argument("--game", required=True, help="test install (never the player's own copy: saves are deleted)")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--dll", default=os.path.join(REPO, "build", "pc-compat", "d3d8.dll"))
    ap.add_argument("--keep-ini", action="store_true", help="do not copy the profile's spartacompat.ini")
    ap.add_argument("--out")
    a = ap.parse_args()
    gdir = os.path.join(REPO, "games", a.serial)
    profile = load_jsonc(os.path.join(gdir, "profile.json"))
    pc = profile.get("pcCompat") or {}
    scenarios = (pc.get("regression") or {}).get("scenarios") or []
    if a.only:
        scenarios = [s for s in scenarios if s["name"] in a.only]
    if not scenarios:
        print("no regression scenarios (profile pcCompat.regression.scenarios)")
        return 2
    game = os.path.abspath(a.game)
    exe = os.path.join(game, pc.get("executable", {}).get("path", ""))
    if not os.path.isfile(exe):
        print("executable not found:", exe)
        return 2
    if a.dll:
        if not os.path.isfile(a.dll):
            print("layer not built:", a.dll)
            return 2
        shutil.copy(a.dll, os.path.join(game, pc.get("proxyDll", "d3d8.dll")))
    if not a.keep_ini:
        shutil.copy(os.path.join(gdir, "spartacompat.ini"), os.path.join(game, "spartacompat.ini"))
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%MZ")
    out = os.path.abspath(a.out or os.path.join(os.path.dirname(game), "regression", stamp))
    os.makedirs(out, exist_ok=True)
    results = []
    for sc in scenarios:
        print("== %s (%d s)" % (sc["name"], sc["seconds"]), flush=True)
        r = run_scenario(sc, game, exe, out)
        results.append(r)
        for c in r["checks"]:
            print("  %-7s %-52s %s" % (c["status"].upper(), c["check"][:52], c["detail"]))
    bad = [(r["name"], c["check"]) for r in results for c in r["checks"] if c["status"] in ("fail", "unknown")]
    summary = {"serial": a.serial, "utc": stamp, "dll": os.path.abspath(a.dll) if a.dll else None,
               "passed": not bad, "failed": bad, "scenarios": results}
    json.dump(summary, open(os.path.join(out, "results.json"), "w", encoding="utf-8"), indent=1)
    print("\n%s: %d scenario(s), %s -> %s" % (a.serial, len(results), "ALL PASSED" if not bad else "%d check(s) failed or undecided" % len(bad),
                                          os.path.join(out, "results.json")))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
