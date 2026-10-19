# Tests for scripts/pc_save_edit.py on a synthetic save built from the Pooh save format (no game data).
import os
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pc_save_edit as se  # noqa: E402

FMT = se.load_format("PC-DPTWP-UK")


def synthetic_save(names=("A",)):
    data = bytearray(FMT["size"])
    for n, name in enumerate(names):
        o = se.slot_offset(FMT, n)
        data[o:o + len(name)] = name.encode("latin-1")
        data[o + 0x1A] = data[o + 0x1B] = data[o + 0x29] = data[o + 0x2A] = 1  # new-profile unlocks
        data[o + 0x48] = 0                                                          # board 1 reached
        struct.pack_into("<I", data, o + 0x4C, 3)                                   # movies 1-2
    struct.pack_into("<I", data, FMT["checksum"]["at"], se.checksum(FMT, data))
    return data


class SaveEditTests(unittest.TestCase):
    def test_synthetic_save_is_valid_and_fields_read(self):
        d = synthetic_save()
        self.assertIsNone(se.validate(FMT, d))
        self.assertEqual(se.slot_name(FMT, d, 0), "A")
        self.assertEqual(se.slot_name(FMT, d, 1), "")
        self.assertEqual(sum(se.read_field(FMT, d, 0, "unlocks")), 4)
        self.assertEqual(se.read_field(FMT, d, 0, "movies"), 3)

    def test_unlock_all_sets_levels_and_movies_and_fixes_the_checksum(self):
        d = synthetic_save(("A", "B"))
        se.apply_preset(FMT, d, 0, "unlock-all")
        self.assertIsNone(se.validate(FMT, d))
        self.assertEqual(se.read_field(FMT, d, 0, "unlocks"), [1] * 25)
        self.assertEqual(se.read_field(FMT, d, 0, "movies"), 127)
        self.assertEqual(se.read_field(FMT, d, 0, "adventureBoard"), 0)  # progress untouched
        self.assertEqual(sum(se.read_field(FMT, d, 1, "unlocks")), 4)  # other profile untouched

    def test_boards_preset_also_sets_the_board(self):
        d = synthetic_save()
        se.apply_preset(FMT, d, 0, "unlock-all-boards")
        self.assertEqual(se.read_field(FMT, d, 0, "adventureBoard"), 2)

    def test_cli_backs_up_and_refuses_a_broken_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "DPTWP.sav")
            open(p, "wb").write(synthetic_save())
            script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pc_save_edit.py")
            r = subprocess.run([sys.executable, script, "apply", "--serial", "PC-DPTWP-UK", "--save", p, "--preset", "unlock-all"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(any(f.startswith("DPTWP.sav.bak-") for f in os.listdir(tmp)))
            self.assertEqual(se.read_field(FMT, bytearray(open(p, "rb").read()), 0, "movies"), 127)
            broken = bytearray(open(p, "rb").read())
            broken[0x200] ^= 1
            open(p, "wb").write(broken)
            r = subprocess.run([sys.executable, script, "apply", "--serial", "PC-DPTWP-UK", "--save", p, "--preset", "unlock-all"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("refusing", r.stdout)


if __name__ == "__main__":
    unittest.main()
