# Tests for scripts/pc_regression.py: profile comments, log facts and checks, blank-frame and hor+ side checks.
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pc_regression as pr  # noqa: E402

LOG = """[    0.008] SpartaRecomp PC compatibility layer loaded into C:\\game\\
[    0.020] patch FrustumHorPlus: applied 7 byte(s) at 0040D133
[    0.740] d3d: 2 device hook(s) had been written back by the runtime (after EndStateBlock); re-applied
[   10.020] [textures] 271 created, 138 uploads, 137 distinct images (1 without a replacement), 137 replacements mapped
[   10.020] [state] 410 frames in the last 10 s (41.0 fps), 410 presented, 274 delta ticks (1/120 s); frame_delta: 1 calls
[   20.020] [state] 1200 frames in the last 10 s (120.0 fps), 1610 presented, 1200 delta ticks (1/120 s); frame_delta: 1 calls
[   20.500] game: CDDHCard::Write:OK
[   26.068] SPARTA_EXIT_AFTER_MS reached (26000 ms): terminating the game
"""


class ProfileTests(unittest.TestCase):
    def test_comments_are_removed_but_not_inside_strings(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "profile.json")
            open(p, "w", encoding="utf-8").write('{\n  // a comment\n  "url": "http://x//y", // trailing\n  "n": 1\n}\n')
            self.assertEqual(pr.load_jsonc(p), {"url": "http://x//y", "n": 1})


class IniTests(unittest.TestCase):
    def test_saved_settings_are_read_like_the_layer_reads_them(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "spartacompat.ini")
            open(p, "w", encoding="utf-8").write("[Display]\n; FpsLimit=30 (a comment)\nfpslimit = 60\n[Textures]\nEnable=0\n")
            self.assertEqual(pr.read_ini_value(p, "display", "FpsLimit"), "60")
            self.assertIsNone(pr.read_ini_value(p, "Display", "Enable"))  # only in [Textures]
            res = pr.check_ini(p, [{"key": "Display.FpsLimit", "value": 60}, {"key": "Textures.Enable", "value": "1"},
                                   {"key": "Overlay.FpsCounter", "value": "1"}])
            self.assertEqual([r[1] for r in res], ["pass", "fail", "fail"])
            self.assertEqual(res[2][2], "missing")
            self.assertEqual(pr.check_ini(os.path.join(d, "absent.ini"), [{"key": "A.B", "value": "1"}])[0][1], "fail")


class LogTests(unittest.TestCase):
    def test_facts_come_from_positive_evidence(self):
        f = pr.parse_log(LOG)
        self.assertTrue(f["loaded"])
        self.assertTrue(f["ended_by_timer"])
        self.assertEqual(f["presented"], 1610)
        self.assertEqual(f["missing_textures"], 1)
        self.assertEqual(f["stalls"], 0)

    def test_stalls_ignore_start_up_and_movies(self):
        log = ("[   10.040] [state] no new frame for 10 s (0 presented); main thread eip=0048039a\n"
               "[   30.000] movie: video window 0,0 3840x2880 -> 760,0 1920x1440\n"
               "[   40.000] [state] no new frame for 10 s (900 presented); main thread eip=0048039a\n"
               "[   80.000] [state] no new frame for 10 s (5000 presented); main thread eip=0048039a\n")
        self.assertEqual(pr.count_stalls(log), 1)  # only the one 50 s after the movie, with frames before it

    def test_a_crashed_run_is_not_a_session(self):
        f = pr.parse_log(LOG.split("[   20.500]")[0])  # log ends early: no timer line
        self.assertFalse(f["ended_by_timer"])

    def test_expected_and_forbidden_lines(self):
        res = dict((n, s) for n, s, _ in pr.check_log(LOG, ["CDDHCard::Write:OK", "Read:OK"], ["refused"]))
        self.assertEqual(res["log has /CDDHCard::Write:OK/"], "pass")
        self.assertEqual(res["log has /Read:OK/"], "fail")
        self.assertEqual(res["log lacks /refused/"], "pass")

    def test_judge_reports_unknown_without_texture_statistics(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "textures"))  # a pack folder exists, but the log has no statistics
            r = pr.judge({"name": "x", "maxMissingTextures": 0}, d, "SpartaRecomp PC compatibility layer loaded", d, 0, 1.0)
            st = {c["check"]: c["status"] for c in r["checks"]}
            self.assertEqual(st["textures without a replacement <= 0"], "unknown")
            self.assertEqual(st["frames presented"], "fail")
            self.assertEqual(st["frame dumps"], "unknown")

    def test_checks_for_an_absent_optional_part_are_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            sc = {"name": "x", "maxMissingTextures": 1, "expectLog": [{"rx": "soft edges", "needs": "textures"}, "loaded"]}
            st = {c["check"]: c["status"] for c in pr.judge(sc, d, LOG, d, 0, 1.0)["checks"]}
            self.assertEqual(st["log has /soft edges/"], "skip")
            self.assertEqual(st["textures without a replacement <= 1"], "skip")
            self.assertEqual(st["log has /loaded/"], "pass")
            os.makedirs(os.path.join(d, "textures"))
            st = {c["check"]: c["status"] for c in pr.judge(sc, d, LOG, d, 0, 1.0)["checks"]}
            self.assertEqual(st["log has /soft edges/"], "fail")  # with a pack the line is required
            self.assertEqual(st["textures without a replacement <= 1"], "pass")

    def test_a_closed_session_must_end_by_itself(self):
        with tempfile.TemporaryDirectory() as d:
            sc = {"name": "x", "closeAfterSeconds": 40}
            check = "closed like Alt+F4, exit 0"
            ok = pr.judge(sc, d, LOG.replace("SPARTA_EXIT_AFTER_MS reached", "x"), d, 0, 41.0, closed=1)
            self.assertEqual({c["check"]: c["status"] for c in ok["checks"]}[check], "pass")
            timer = pr.judge(sc, d, LOG, d, 0, 60.0, closed=1)  # still running until the layer's timer stopped it
            self.assertEqual({c["check"]: c["status"] for c in timer["checks"]}[check], "fail")
            nowin = pr.judge(sc, d, LOG.replace("SPARTA_EXIT_AFTER_MS reached", "x"), d, 0, 41.0, closed=0)
            self.assertEqual({c["check"]: c["status"] for c in nowin["checks"]}[check], "fail")
            crash = pr.judge(sc, d, LOG.replace("SPARTA_EXIT_AFTER_MS reached", "x"), d, 3221225477, 41.0, closed=1)
            self.assertEqual({c["check"]: c["status"] for c in crash["checks"]}[check], "fail")


class FrameTests(unittest.TestCase):
    def test_blank_and_drawn_frames(self):
        from PIL import Image, ImageDraw
        self.assertTrue(pr.is_blank(Image.new("RGB", (64, 48), (255, 0, 255))))
        two = Image.new("RGB", (64, 48), (0, 0, 0))
        ImageDraw.Draw(two).rectangle((0, 0, 31, 47), fill=(255, 255, 255))
        self.assertTrue(pr.is_blank(two))  # two colours only
        drawn = Image.new("RGB", (64, 48))
        drawn.putdata([(x * 4 % 256, y * 5 % 256, (x + y) % 256) for y in range(48) for x in range(64)])
        self.assertFalse(pr.is_blank(drawn))

    def test_side_black_share_counts_only_the_side_strips(self):
        from PIL import Image, ImageDraw
        img = Image.new("RGB", (344, 144), (0, 0, 0))  # sides black
        ImageDraw.Draw(img).rectangle((76, 0, 267, 143), fill=(200, 150, 100))  # centred 4:3 part drawn
        self.assertGreater(pr.side_black_share(img, 192 / 344), 0.95)
        full = Image.new("RGB", (344, 144), (90, 120, 60))
        self.assertEqual(pr.side_black_share(full, 192 / 344), 0.0)


if __name__ == "__main__":
    unittest.main()
