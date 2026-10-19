"""Save editor for pc-compat games: shows profiles and applies presets (e.g. "unlock-all") described as data.

The layout comes from games/<SERIAL>/save-format.json: file size, checksum rule, profile slots, named fields and
presets (lists of writes: "set" a value on every byte/element of a field, or "or" bits into it). Before any change the
original is copied to <save>.bak-<UTC time>; after writing, the file is read back and its checksum verified.

usage:
  python scripts/pc_save_edit.py show  --serial PC-DPTWP-UK --save "<game dir>\\DPTWP.sav"
  python scripts/pc_save_edit.py apply --serial PC-DPTWP-UK --save "<game dir>\\DPTWP.sav" --preset unlock-all
                                       [--slot N | --name NAME | --all] [--dry-run]
The save is the player's own data: edit only when the game is closed (it rewrites the whole file when saving).
"""
import argparse
import json
import os
import shutil
import struct
import sys
from datetime import datetime, timezone

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = {"u8": (1, "<B"), "u16": (2, "<H"), "u32": (4, "<I")}


def load_format(serial):
    with open(os.path.join(REPO, "games", serial, "save-format.json"), encoding="utf-8") as f:
        return json.load(f)


def checksum(fmt, data):
    c = fmt["checksum"]
    if c["type"] != "byte-sum-u32le":
        raise ValueError("unknown checksum type %s" % c["type"])
    return sum(data[c["from"]:c["to"]]) & 0xFFFFFFFF


def stored_checksum(fmt, data):
    return struct.unpack_from("<I", data, fmt["checksum"]["at"])[0]


def slot_offset(fmt, n):
    s = fmt["slots"]
    return s["offset"] + n * s["size"]


def slot_name(fmt, data, n):
    nm = fmt["slots"]["name"]
    o = slot_offset(fmt, n) + nm["offset"]
    raw = data[o:o + nm["size"]].split(b"\0")[0]
    return raw.decode("latin-1")


def read_field(fmt, data, n, name):
    f = fmt["fields"][name]
    size, code = SIZES[f["type"]]
    o = slot_offset(fmt, n) + f["offset"]
    count = f.get("size", 1) if f["type"] == "u8" else 1
    vals = [struct.unpack_from(code, data, o + i * size)[0] for i in range(count)]
    return vals if count > 1 else vals[0]


def apply_preset(fmt, data, n, preset):
    """Returns a list of (field, before, after) for one slot; data (bytearray) is changed in place."""
    changes = []
    for w in fmt["presets"][preset]["writes"]:
        f = fmt["fields"][w["field"]]
        size, code = SIZES[f["type"]]
        o = slot_offset(fmt, n) + f["offset"]
        count = f.get("size", 1) if f["type"] == "u8" else 1
        before = read_field(fmt, data, n, w["field"])
        for i in range(count):
            cur = struct.unpack_from(code, data, o + i * size)[0]
            new = w["set"] if "set" in w else cur | w["or"]
            struct.pack_into(code, data, o + i * size, new)
        changes.append((w["field"], before, read_field(fmt, data, n, w["field"])))
    struct.pack_into("<I", data, fmt["checksum"]["at"], checksum(fmt, data))
    return changes


def validate(fmt, data):
    if len(data) != fmt["size"]:
        return "size %d, expected %d" % (len(data), fmt["size"])
    if stored_checksum(fmt, data) != checksum(fmt, data):
        return "checksum %08x does not match the contents (%08x)" % (stored_checksum(fmt, data), checksum(fmt, data))
    return None


def cmd_show(fmt, data):
    print("checksum %08x (%s)" % (stored_checksum(fmt, data), "ok" if not validate(fmt, data) else validate(fmt, data)))
    for n in range(fmt["slots"]["count"]):
        name = slot_name(fmt, data, n)
        if not name:
            print("slot %d: empty" % n)
            continue
        print("slot %d: %s" % (n, name))
        for fname in fmt["fields"]:
            v = read_field(fmt, data, n, fname)
            print("    %-20s %s" % (fname, "".join(str(x) for x in v) if isinstance(v, list) else v))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["show", "apply"])
    ap.add_argument("--serial", required=True)
    ap.add_argument("--save", required=True)
    ap.add_argument("--preset")
    ap.add_argument("--slot", type=int)
    ap.add_argument("--name")
    ap.add_argument("--all", action="store_true", help="every used slot")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    fmt = load_format(a.serial)
    with open(a.save, "rb") as f:
        data = bytearray(f.read())
    problem = validate(fmt, data)
    if a.command == "show":
        cmd_show(fmt, data)
        return 0 if not problem else 1
    if problem:
        print("refusing to edit:", problem)
        return 1
    if not a.preset or a.preset not in fmt["presets"]:
        print("--preset must be one of:", ", ".join(fmt["presets"]))
        return 2
    used = [n for n in range(fmt["slots"]["count"]) if slot_name(fmt, data, n)]
    if a.all:
        slots = used
    elif a.slot is not None:
        slots = [a.slot] if a.slot in used else []
    elif a.name is not None:
        slots = [n for n in used if slot_name(fmt, data, n) == a.name]
    else:
        slots = used if len(used) == 1 else []
    if not slots:
        print("no matching profile (used slots: %s); pick one with --slot, --name or --all" %
              ", ".join("%d=%s" % (n, slot_name(fmt, data, n)) for n in used))
        return 2
    print(fmt["presets"][a.preset]["doc"])
    for n in slots:
        for field, before, after in apply_preset(fmt, data, n, a.preset):
            fmt_v = lambda v: "".join(str(x) for x in v) if isinstance(v, list) else str(v)  # noqa: E731
            print("  slot %d (%s) %-16s %s -> %s" % (n, slot_name(fmt, data, n), field, fmt_v(before), fmt_v(after)))
    if a.dry_run:
        print("dry run: nothing written")
        return 0
    backup = "%s.bak-%s" % (a.save, datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    shutil.copy2(a.save, backup)
    with open(a.save, "wb") as f:
        f.write(data)
    with open(a.save, "rb") as f:
        check = validate(fmt, f.read())
    if check:
        shutil.copy2(backup, a.save)
        print("written file failed its check (%s): original restored from %s" % (check, backup))
        return 1
    print("saved; original kept as", backup)
    return 0


if __name__ == "__main__":
    sys.exit(main())
