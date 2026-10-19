"""Checks a pc-compat game's setup program against the user's own disc image, unattended (no window, no shortcuts).

Steps: install from the image (.iso), from a raw image made from it (.bin, 2352-byte sectors) and from a folder (what a
CD drive looks like), each compared file by file with a reference copy; update an existing copy (user settings, the
game's options and the save stay, the layer and non-user keys are replaced, no disc needed); refuse a folder that is
not the game, a missing image and a file that is no disc image. Everything it writes goes under --work (game data:
never inside the repository) and is deleted again, except the result lines.

usage: python scripts/pc_installer_check.py --setup build/pc-installer/<name>.exe --iso <own disc image>
                                            --reference <a prepared copy of the game> --work <scratch folder>
                                            [--folder <the disc's files as a folder>]
Exit code 0 = every check passed.
"""
import argparse
import ctypes
import hashlib
import os
import shutil
import subprocess
import sys

# the fix's own files and per-session files differ between copies by design
IGNORE_TOP = {"d3d8.dll", "spartacompat.ini", "registry.ini", "spartacompat.log", "spartacompat.prev.log", "fix-install.log",
              "how to play (fix).txt", "dptwp.sav", "spartacompat.testmute", "licenses", "textures"}


def tree(root):
    out = {}
    for d, _, files in os.walk(root):
        rel_d = os.path.relpath(d, root)
        if rel_d.split(os.sep)[0].lower() in IGNORE_TOP:
            continue
        for f in files:
            if rel_d == "." and (f.lower() in IGNORE_TOP or f.lower().startswith("dptwp.sav")):
                continue
            p = os.path.join(d, f)
            out[os.path.normpath(os.path.join(rel_d, f)).lower()] = hashlib.sha256(open(p, "rb").read()).hexdigest()
    return out


def compare(a, b):
    ta, tb = tree(a), tree(b)
    diff = [k for k in set(ta) | set(tb) if ta.get(k) != tb.get(k)]
    return not diff, "%d vs %d files, %d different%s" % (len(ta), len(tb), len(diff), (": " + ", ".join(sorted(diff)[:5])) if diff else "")


def iso_to_bin(src, dst):
    sync = bytes([0] + [0xFF] * 10 + [0])
    bcd = lambda n: ((n // 10) << 4) | (n % 10)
    with open(src, "rb") as f, open(dst, "wb") as o:
        lba = 0
        while True:
            data = f.read(2048)
            if not data:
                break
            a = lba + 150
            o.write(sync + bytes([bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 1]) + data.ljust(2048, b"\0") + bytes(288))
            lba += 1


def ini_get(path, sec, key):
    buf = ctypes.create_unicode_buffer(256)
    ctypes.windll.kernel32.GetPrivateProfileStringW(sec, key, "<none>", buf, 256, path)
    return buf.value


def ini_set(path, sec, key, value):
    ctypes.windll.kernel32.WritePrivateProfileStringW(sec, key, value, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--setup", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--folder")
    a = ap.parse_args()
    a.setup = os.path.abspath(a.setup)  # CreateProcess does not resolve relative paths with forward slashes
    results = []

    def run(*args):
        return subprocess.run([a.setup, *args, "/quiet", "/noshortcuts"], capture_output=True).returncode

    def check(name, ok, detail=""):
        results.append(ok)
        print("%-4s %-44s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)

    os.makedirs(a.work, exist_ok=True)
    t = os.path.join(a.work, "check-iso")
    shutil.rmtree(t, ignore_errors=True)
    code = run("/source", a.iso, "/target", t)
    same, info = compare(t, a.reference) if code == 0 else (False, "")
    check("install from the disc image (.iso)", code == 0 and same, "exit %d; %s" % (code, info))

    binp = os.path.join(a.work, "check.bin")
    iso_to_bin(a.iso, binp)
    tb = os.path.join(a.work, "check-bin")
    shutil.rmtree(tb, ignore_errors=True)
    code = run("/source", binp, "/target", tb)
    same, info = compare(tb, a.reference) if code == 0 else (False, "")
    check("install from a raw image (.bin)", code == 0 and same, "exit %d; %s" % (code, info))
    os.remove(binp)
    shutil.rmtree(tb, ignore_errors=True)

    if a.folder:
        tf = os.path.join(a.work, "check-folder")
        shutil.rmtree(tf, ignore_errors=True)
        code = run("/source", a.folder, "/target", tf)
        same, info = compare(tf, a.reference) if code == 0 else (False, "")
        check("install from a folder (CD drive)", code == 0 and same, "exit %d; %s" % (code, info))
        shutil.rmtree(tf, ignore_errors=True)

    if results[0]:  # update the .iso copy
        ini, reg, sav = (os.path.join(t, n) for n in ("spartacompat.ini", "registry.ini", "DPTWP.sav"))
        ini_set(ini, "Display", "FpsLimit", "60")
        ini_set(ini, "Overlay", "FpsCounter", "1")
        ini_set(ini, "Patches", "FrameCap30", "tampered")
        with open(reg, "a", encoding="ascii") as f:
            f.write("NbPlayer=dword:0x00000003\n")
        open(os.path.join(t, "d3d8.dll"), "wb").write(b"old")
        open(sav, "wb").write(b"save")
        code = run("/target", t)
        log = open(os.path.join(t, "fix-install.log"), encoding="utf-8-sig").read()
        check("update without the disc", code == 0 and "Updating the fix" in log, "exit %d" % code)
        check("update keeps user settings", ini_get(ini, "Display", "FpsLimit") == "60" and ini_get(ini, "Overlay", "FpsCounter") == "1")
        check("update resets non-user keys", ini_get(ini, "Patches", "FrameCap30") != "tampered")
        check("update keeps the game's options and the save", "0x00000003" in open(reg).read() and open(sav, "rb").read() == b"save")
        check("update replaces the layer", os.path.getsize(os.path.join(t, "d3d8.dll")) > 100000)
    shutil.rmtree(t, ignore_errors=True)

    tw = os.path.join(a.work, "check-wrong")
    shutil.rmtree(tw, ignore_errors=True)
    os.makedirs(os.path.join(a.work, "check-not-a-game"), exist_ok=True)
    open(os.path.join(a.work, "check-not-a-game", "readme.txt"), "w").write("x")
    check("refuses a folder that is not the game", run("/source", os.path.join(a.work, "check-not-a-game"), "/target", tw) == 3)
    check("reports a missing image", run("/source", os.path.join(a.work, "no-such.iso"), "/target", tw) == 2)
    bad = os.path.join(a.work, "check-bad.iso")
    open(bad, "wb").write(os.urandom(40 * 2048))
    check("rejects a file that is no disc image", run("/source", bad, "/target", tw) == 2)
    os.remove(bad)
    shutil.rmtree(tw, ignore_errors=True)
    shutil.rmtree(os.path.join(a.work, "check-not-a-game"), ignore_errors=True)
    print("%d of %d checks passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
