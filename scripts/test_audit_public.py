# Tests for scripts/audit_public.py: game data, binaries, private strings, extra patterns and dashes are found.
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_public as ap  # noqa: E402

# test strings are assembled so this file does not trip the audit itself
USER_PATH = "C:" + "\\Users\\" + "someone\\game"
LAN_ADDRESS = "192." + "168.1.20"
DASH = "\u2014"


class AuditTests(unittest.TestCase):
    def write(self, root, rel, data):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data if isinstance(data, bytes) else data.encode("utf-8"))

    def test_clean_repository_passes(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "README.md", "# Fix\nInstall it to %USERPROFILE%\\Games, then play.\n")
            self.write(d, "docs/x.md", "Default folder: C:\\Users\\%USERNAME%\\Games\n")
            self.write(d, "native/src/main.cpp", "int main() { return 0; }\n")
            self.write(d, ".git/objects/x.bin", b"\0\1")  # git internals are not published files
            self.assertEqual(ap.audit(d), [])

    def test_game_data_and_binaries_are_found(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "WTP.exe", b"MZ")
            self.write(d, "textures/abc.png", b"\x89PNG")
            self.write(d, "notes.txt", b"text\0with a NUL")
            self.write(d, "big.txt", "x" * (ap.MAX_BYTES + 1))
            found = {f[0].replace("\\", "/") for f in ap.audit(d)}
            self.assertEqual(found, {"WTP.exe", "textures/abc.png", "notes.txt", "big.txt"})

    def test_private_strings_and_dashes_are_found(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "a.ini", "Root=%s\n" % USER_PATH)
            self.write(d, "b.py", "HOST = '%s'\n" % LAN_ADDRESS)
            self.write(d, "c.md", "A dash %s here\n" % DASH)
            self.assertEqual(sorted(f[0] for f in ap.audit(d)), ["a.ini", "b.py", "c.md"])

    def test_extra_patterns_come_from_a_file(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "repo/x.md", "made on secret-machine-7\n")
            self.write(d, "rules.txt", "# private names\nsecret-machine-\\d\ta private machine\n")
            rules = ap.load_patterns(os.path.join(d, "rules.txt"))
            self.assertEqual(len(rules), 1)
            self.assertEqual(ap.audit(os.path.join(d, "repo")), [])
            self.assertEqual(len(ap.audit(os.path.join(d, "repo"), extra=rules)), 1)

    def test_files_the_gitignore_excludes_are_not_published(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, ".gitignore", "build/\n*.log\n")
            self.write(d, "build/x/setup.exe", b"MZ")
            self.write(d, "build/gen/payload.rc", "1 24 \"%s\"\n" % USER_PATH)
            self.write(d, "run.log", "x")
            self.write(d, "src/a.cpp", "int a;\n")
            self.assertEqual(ap.audit(d), [])
            self.write(d, "WTP.exe", b"MZ")  # not ignored: found
            self.assertEqual([f[0] for f in ap.audit(d)], ["WTP.exe"])

    def test_screenshots_only_in_docs_images_and_small(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "docs/images/gameplay.jpg", b"\xff\xd8" + b"\0" * 1000)
            self.assertEqual(ap.audit(d), [])
            self.write(d, "docs/images/huge.png", b"\x89PNG" + b"\0" * (ap.MAX_SCREENSHOT_BYTES + 1))
            self.write(d, "docs/other/texture.png", b"\x89PNG")
            self.write(d, "docs/images/sub/x.png", b"\x89PNG")  # only directly in docs/images
            found = sorted(f[0].replace("\\", "/") for f in ap.audit(d))
            self.assertEqual(found, ["docs/images/huge.png", "docs/images/sub/x.png", "docs/other/texture.png"])

    def test_allow_list_exempts_a_file(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "x.md", "A dash %s here\n" % DASH)
            self.assertEqual(ap.audit(d, ["x.md"]), [])


if __name__ == "__main__":
    unittest.main()
