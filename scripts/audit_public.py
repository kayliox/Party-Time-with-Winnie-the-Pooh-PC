"""Checks a folder that is about to be published (a public game-fix repository) for game data and private information.

Findings: files of game-data types (executables, disc images, movies, audio, images, saves, archives; small README
screenshots are allowed in docs/images only, at most 1 MB each), binary or large
files, private strings (local user paths, private network addresses, plus any rules from --patterns), and em/en
dashes in published text. Exit code 0 = clean, 1 = findings (each printed with file and line), 2 = bad arguments.

usage: python scripts/audit_public.py <folder> [--allow <relative path> ...] [--patterns <file>]
A patterns file has one rule per line: a regular expression, a tab, and the reason (lines starting with # are skipped).
Project-specific names belong in such a file kept outside the public repository, never in this script.
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys

GAME_DATA_EXT = {".exe", ".dll", ".iso", ".bin", ".cue", ".img", ".mdf", ".mds", ".nrg", ".ccd", ".dat", ".str", ".mpg",
                 ".mpeg", ".avi", ".wav", ".mp3", ".ogg", ".xa", ".vag", ".tga", ".png", ".jpg", ".jpeg", ".bmp", ".dds",
                 ".tim", ".sav", ".mcr", ".cab", ".hdr", ".chm", ".wri", ".7z", ".zip", ".rar", ".lib", ".a", ".o", ".obj",
                 ".pdb", ".res", ".ico", ".log"}
MAX_BYTES = 512 * 1024
# README screenshots (the game's own frame dumps, small JPEG/PNG) are the one kind of image allowed, and only here
SCREENSHOT_DIR = os.path.join("docs", "images")
SCREENSHOT_EXT = {".jpg", ".jpeg", ".png"}
MAX_SCREENSHOT_BYTES = 1024 * 1024
GENERIC = [
    (r"[A-Za-z]:[\\/]Users[\\/](?!%)[^\\/\s]+", "a local user path"),
    (r"/[a-z]/Users/[^/\s]+", "a local user path"),
    (r"\b(?:192\.168|10\.\d+\.\d+|172\.(?:1[6-9]|2\d|3[01]))\.\d+\.\d+\b", "a private network address"),
]
DASHES = re.compile("[–—]")
TEXT_EXT = {".md", ".txt"}
SKIP_DIRS = {".git"}


def load_patterns(path):
    rules = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\r\n")
        if line and not line.startswith("#") and "\t" in line:
            rx, why = line.split("\t", 1)
            rules.append((rx, why))
    return rules


def published_files(root):
    """The files a commit would publish: git's own list in a git repository, otherwise every file except what the
    root .gitignore excludes (directory entries "name/" and file globs such as "*.log"), so local build outputs and
    game data kept beside the sources (both ignored) do not count."""
    if os.path.isdir(os.path.join(root, ".git")):
        try:
            out = subprocess.run(["git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                                 capture_output=True, check=True).stdout
            return [os.path.normpath(p.decode("utf-8")) for p in out.split(b"\0") if p]
        except (OSError, subprocess.CalledProcessError):
            pass
    ignored_dirs, ignored_globs = set(SKIP_DIRS), []
    gi = os.path.join(root, ".gitignore")
    if os.path.isfile(gi):
        for line in open(gi, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("!"):
                continue
            if line.endswith("/"):
                ignored_dirs.add(line.strip("/"))
            elif "/" not in line:
                ignored_globs.append(line)
    files = []
    for d, dirs, names in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ignored_dirs]
        for f in names:
            if not any(fnmatch.fnmatch(f, g) for g in ignored_globs):
                files.append(os.path.relpath(os.path.join(d, f), root))
    return files


def audit(root, allow=(), extra=()):
    rules = GENERIC + list(extra)
    findings = []
    allow = {os.path.normcase(os.path.normpath(a)) for a in allow}
    for rel in published_files(root):
        if os.path.normcase(rel) in allow:
            continue
        path = os.path.join(root, rel)
        ext = os.path.splitext(rel)[1].lower()
        if ext in SCREENSHOT_EXT and os.path.dirname(os.path.normpath(rel)) == SCREENSHOT_DIR:
            if os.path.getsize(path) > MAX_SCREENSHOT_BYTES:
                findings.append((rel, 0, "screenshot larger than %d KB" % (MAX_SCREENSHOT_BYTES // 1024)))
            continue
        if ext in GAME_DATA_EXT:
            findings.append((rel, 0, "file type %s is not allowed (game data or a build output)" % ext))
            continue
        if os.path.getsize(path) > MAX_BYTES:
            findings.append((rel, 0, "larger than %d KB" % (MAX_BYTES // 1024)))
            continue
        data = open(path, "rb").read()
        if b"\0" in data[:8192]:
            findings.append((rel, 0, "binary file"))
            continue
        text = data.decode("utf-8", errors="replace")
        for n, line in enumerate(text.splitlines(), 1):
            for rx, why in rules:
                if re.search(rx, line):
                    findings.append((rel, n, "contains %s: %s" % (why, line.strip()[:100])))
            if ext in TEXT_EXT and DASHES.search(line):
                findings.append((rel, n, "em/en dash in published text"))
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--allow", nargs="*", default=[], help="relative paths exempt from the checks")
    ap.add_argument("--patterns", help="file with more rules (regex TAB reason), kept outside the public repository")
    a = ap.parse_args()
    if not os.path.isdir(a.folder):
        print("not a folder:", a.folder)
        return 2
    findings = audit(a.folder, a.allow, load_patterns(a.patterns) if a.patterns else ())
    for rel, n, why in findings:
        print("%s%s: %s" % (rel, ":%d" % n if n else "", why))
    print("%d finding(s) in %s" % (len(findings), a.folder))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
