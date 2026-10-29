"""Crash dumps against the boot (#138, agent F): analyse_selftest.py --collect dates each ux0:data/*.psp2dmp by the
Vita's clock, from its name and its LIST time, against when the boot the collect recorded began (boot.txt's LIST time
and the previous boot's kernel.prev.txt). "before" needs a date before that, the clock checked for this boot (kernel.txt's
last line carries the uptime: its LIST time less it is the boot's start by the clock then) and a pid this boot's
kernel.txt never named; test.3's clock ran about three days behind. install_ftp.sh --collect then offers, in a terminal, to archive the dumps from
before the boot off the Vita, and --archive-dumps does it: each checked against the collect's copy, DELE alone.

test.3's own records are the fixture (fixtures/collect-test3-dumps: its ux0:data listing's ten dump lines, the
overlay's own listings, boot.txt, kernel.txt and kernel.prev.txt, COLLECT.json's records of them; none of the dumps)."""
import calendar
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ftp_mock import FtpMock  # noqa: E402
from profile_fixture import stage_profile_builder  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import analyse_selftest  # noqa: E402
import collect_vita  # noqa: E402

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "collect-test3-dumps"
MON = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


def unix(text):
    """'2026-10-20 12:00:30' (the Vita's clock, UTC) -> Unix seconds."""
    return calendar.timegm(time.strptime(text, "%Y-%m-%d %H:%M:%S"))


def list_date(t, year=2026):
    """libftpvita's LIST time of Unix time t when the Vita's clock is in `year`."""
    g = time.gmtime(t)
    return f"{MON[g.tm_mon - 1]} {g.tm_mday:<2d} " + (time.strftime("%H:%M", g) if g.tm_year == year else str(g.tm_year))


def dump_name(t, pid, exe):
    return f"psp2core-{t}-0x{pid:010x}-{exe}.psp2dmp"


# kernel.txt of a boot whose SceShell was 0x118FF and which started PCSG00415 as pid 0x20000; the previous boot's
# SceShell was 0x11901. Its last line is the game's suspend 90 minutes into the boot (t=5400000 ms), and Folder's
# kernel.txt is last written at 13:30: by the clock, this boot began at 12:00, inside the window (clock_check)
KERNEL_HEAD = ("boot guard: boot 1 normal (previous: none, stage none, failures 0) write 0x00000000\n"
               "hooks: display 0, input 0, procevent 0 (0 = ok)\nVita JP Overlay kernel 22 started (normal mode)\n"
               "boot guard: stage shell\nshell registered: pid 0x118FF (was 0x0)\nboot guard: stage worker\n"
               "foreground 0x3269D -> 0x0 (suspend) t=13512 ms\nboot guard: stage ok\n"
               "worker for PCSG00415 (pid 0x20000): ON in games.ini: ur0:tai/VitaJPOverlay_OCR.suprx 0x00030000, start 0\n"
               "game pid 0x20000 active t=93670 ms\n")


def kernel_text(last_ms=5_400_000, extra=""):
    """KERNEL_HEAD, extra lines, and the game's suspend at last_ms (the worker's "game exit" after it)."""
    return KERNEL_HEAD + extra + f"foreground 0x20000 -> 0x0 (suspend) t={last_ms} ms\ngame exit\n"


KERNEL = kernel_text()
KERNEL_PREV = "boot guard: stage shell\nshell registered: pid 0x11901 (was 0x0)\nboot guard: stage ok\ngame exit\n"
BOOT_OK = "vjoboot1 seq=1 mode=0 stage=4 inert=0 fault=0 failures=0 api=22 prev=0/0/0/0\n"


class Folder:
    """A collect folder as tools/collect_vita.py writes one: listings/, the records, COLLECT.json."""

    def __init__(self, path, boot=BOOT_OK, kernel=KERNEL, kernel_prev=KERNEL_PREV, times=None, collected_at=None):
        self.path = Path(path)
        # this boot: the previous boot's last writes at 12:00, boot.txt's ok mark at 12:01, its last kernel.txt
        # line at 13:30 (the Vita's clock)
        self.times = times if times is not None else {
            "ur0:/data/VitaJPOverlay": {"boot.txt": unix("2026-10-20 12:01:10")},
            "ux0:/data/VitaJPOverlay": {"kernel.prev.txt": unix("2026-10-20 12:00:20"),
                                        "status.prev.txt": unix("2026-10-20 12:00:05"),
                                        "kernel.txt": unix("2026-10-20 13:30:40"),
                                        "status.txt": unix("2026-10-20 12:40:00")},
            "ux0:/data": {}}
        self.files = {}
        if boot is not None:
            self.write("ur0/data/VitaJPOverlay/boot.txt", boot)
        if kernel is not None:
            self.write("ux0/data/VitaJPOverlay/kernel.txt", kernel)
        if kernel_prev is not None:
            self.write("ux0/data/VitaJPOverlay/kernel.prev.txt", kernel_prev)
        self.collected_at = collected_at or "2026-10-23T09:00:00Z"
        self.save()

    def write(self, rel, text):
        p = self.path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def dump(self, t, pid, exe, size=4096, fetched=True, listed=True, written=None):
        name = dump_name(t, pid, exe)
        if listed:
            self.times["ux0:/data"][name] = (t + 30 if written is None else written, size)
        if fetched:
            data = (name.encode() * (size // len(name) + 1))[:size]
            self.write("ux0/data/" + name, "")
            (self.path / "ux0/data" / name).write_bytes(data)
            self.files[f"ux0:/data/{name}"] = {"local": f"ux0/data/{name}", "bytes": size,
                                               "sha256": hashlib.sha256(data).hexdigest()}
        self.save()
        return name

    def save(self):
        for device_path, entries in self.times.items():
            rows = []
            for name, value in entries.items():
                t, size = value if isinstance(value, tuple) else (value, 77)
                rows.append(f"-rw-r--r-- 1 vita vita {size} {list_date(t)} {name}")
            listing = self.path / "listings" / (device_path.replace(":/", "_").replace("/", "_") + ".txt")
            listing.parent.mkdir(parents=True, exist_ok=True)
            listing.write_text("".join(r + "\n" for r in rows))
        (self.path / "COLLECT.json").write_text(json.dumps({"collected_at": self.collected_at, "host": "192.0.2.1:1337",
                                                            "files": self.files, "missing": [], "errors": []}))

    def dumps(self):
        record = json.loads((self.path / "COLLECT.json").read_text())
        return analyse_selftest.summarise_dumps(self.path, record)


class ListTimes(unittest.TestCase):
    def test_libftpvita_list_dates(self):
        self.assertEqual(analyse_selftest.list_span("Oct 10 06:38", 2026), (unix("2026-10-10 06:38:00"),
                                                                            unix("2026-10-10 06:38:59")))
        self.assertEqual(analyse_selftest.list_span("Oct 7 18:49", 2026)[0], unix("2026-10-07 18:49:00"))
        # a file of another year than the Vita's: the whole day
        self.assertEqual(analyse_selftest.list_span("Oct 10 2025", 2026), (unix("2025-10-10 00:00:00"),
                                                                           unix("2025-10-10 23:59:59")))
        self.assertIsNone(analyse_selftest.list_span("Oct 10 06:38", None))  # no year to put it in
        self.assertIsNone(analyse_selftest.list_span("Feb 30 06:38", 2026))
        self.assertIsNone(analyse_selftest.list_span("Foo 3 06:38", 2026))

    def test_the_listing_as_libftpvita_writes_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "listings").mkdir()
            (Path(tmp) / "listings/ux0_data.txt").write_text(
                "drwxr-xr-x 1 vita vita 32768 Oct 7  18:49 VitaDB\n"
                "-rw-r--r-- 1 vita vita 1327440 Oct 10 06:38 psp2core-1791614280-0x0000011905-shell.self.psp2dmp\n"
                "-rw-r--r-- 1 vita vita 12 Dec 31 2025 a b.txt\n")
            got = analyse_selftest.listing(tmp, "ux0:/data")
        self.assertEqual(got, {"VitaDB": {"dir": True, "bytes": 32768, "date": "Oct 7 18:49"},
                               "psp2core-1791614280-0x0000011905-shell.self.psp2dmp":
                                   {"dir": False, "bytes": 1327440, "date": "Oct 10 06:38"},
                               "a b.txt": {"dir": False, "bytes": 12, "date": "Dec 31 2025"}})

    def test_the_collector_records_each_files_list_time(self):
        # COLLECT.json's listings carry the LIST time as listed (the day without libftpvita's padding)
        entries = collect_vita.Collector.parse_list(["-rw-r--r-- 1 vita vita 1327440 Oct 10 06:38 psp2core-1.psp2dmp\r",
                                                     "drwxr-xr-x 1 vita vita 32768 Oct 7  18:49 VitaDB",
                                                     "-rw-r--r-- 1 vita vita 5 Jan 02 2025 old  name.txt"])
        self.assertEqual(entries, [{"name": "psp2core-1.psp2dmp", "bytes": 1327440, "dir": False, "date": "Oct 10 06:38"},
                                   {"name": "VitaDB", "bytes": 32768, "dir": True, "date": "Oct 7 18:49"},
                                   {"name": "old  name.txt", "bytes": 5, "dir": False, "date": "Jan 2 2025"}])


class Test3(unittest.TestCase):
    """test.3's collect: ten dumps, none from its boot. The last one is dated after that boot began by a clock that
    was set back about three days since; its SceShell pid is not the boot's (0x118FF)."""

    def setUp(self):
        record = json.loads((FIXTURE / "COLLECT.json").read_text())
        self.d = analyse_selftest.summarise_dumps(FIXTURE, record)

    def test_every_dump_is_from_before_test3s_boot(self):
        d = self.d
        self.assertEqual(d["year"], 2026)
        self.assertEqual(d["year_from"], "the crash dumps' names")
        # the previous boot's last kernel.txt write (07:01, less 2 minutes), boot.txt's ok mark (07:02, 55 s or more in)
        self.assertEqual((d["boot"]["from"], d["boot"]["to"]), ("2026-10-13 06:59:00", "2026-10-13 07:02:04"))
        self.assertFalse(d["boot"]["estimate"])
        # the clock, checked at kernel.txt's last write (07:15, its last line's t=878340 ms): the boot began at
        # 07:00:21-07:01:21 by it, inside the window
        self.assertEqual(d["boot"]["clock"]["state"], "checked")
        self.assertIn("put this boot's start at 2026-10-13 07:00:21 to 2026-10-13 07:01:21", d["boot"]["clock"]["why"])
        self.assertEqual(len(d["entries"]), 10)
        self.assertEqual([e["when"] for e in d["entries"]], ["before"] * 9 + ["unclear"])
        self.assertEqual([e["crashed"] for e in d["entries"]][:2], ["2026-10-10 06:38:00", "2026-10-10 07:03:36"])
        self.assertTrue(all(e["fetched"] for e in d["entries"]))
        self.assertEqual([e["why"] for e in d["entries"][:9]], ["dated before this boot began"] * 9)
        # the last one, dated after this boot began by a clock now three days behind: not this boot's SceShell, but it
        # could be a later boot's that the overlay did not record as well as an earlier one's
        last = d["entries"][-1]
        self.assertEqual((last["name"], last["crashed"], last["process"], last["pid"]),
                         ("psp2core-1791900588-0x00000118db-shell.self.psp2dmp", "2026-10-13 14:09:48", "SceShell",
                          "0x118DB"))
        self.assertEqual(last["why"], "dated after this boot began, but its SceShell (pid 0x118DB) is not this boot's "
                                      "(kernel.txt registered 0x118FF): an earlier boot's under a clock set back since "
                                      "it crashed, or a later boot's that the overlay did not record (one started with "
                                      "L held)")
        self.assertEqual(d["entries"][3]["process"], "an app or game (eboot.bin)")
        self.assertEqual(d["newest"], "2026-10-13 07:15:59")

    def test_the_summary_says_so(self):
        text = "\n".join(analyse_selftest.render_dumps(self.d))
        self.assertIn("  crash dumps: 10 on the card, none from this boot, 9 from before it, 1 not placed; this boot "
                      "began between "
                      "2026-10-13 06:59:00 and 2026-10-13 07:02:04 by the Vita's clock (after the previous boot's last "
                      "kernel.txt write (kernel.prev.txt, less 2 minutes), 55 s or more before boot.txt's ok mark)", text)
        self.assertIn("    before this boot 2026-10-10 06:38:00  SceShell pid 0x11905, 1,327,440 B: "
                      "psp2core-1791614280-0x0000011905-shell.self.psp2dmp (dated before this boot began)", text)
        self.assertIn("    the Vita's clock, checked for this boot: kernel.txt's last uptime line (t=878,340 ms) and its "
                      "last write (2026-10-13 07:15 by the clock) put this boot's start at 2026-10-13 07:00:21 to "
                      "2026-10-13 07:01:21\n", text + "\n")
        self.assertIn("    the Vita's clock: this boot's newest kernel.txt write is dated 2026-10-13 07:15 by it; this "
                      "computer's clock read 2026-10-16T07:17:43Z at the collect", text)
        self.assertNotIn("NEW", text)

    def test_the_whole_collect_analysis(self):
        r = subprocess.run([sys.executable, str(ROOT / "tools/analyse_selftest.py"), "--collect", str(FIXTURE)],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("crash dumps: 10 on the card, none from this boot, 9 from before it, 1 not placed", r.stdout)
        self.assertIn("  - 1 crash dump on the card is from this boot or not placed (above): the analyser does not decode",
                      r.stdout)
        self.assertIn("  - 9 crash dumps on the card are from earlier boots (above): install_ftp.sh --collect offers to "
                      "archive them off the Vita, this collect keeps them", r.stdout)
        self.assertNotIn("the analyser cannot decode", r.stdout)
        j = subprocess.run([sys.executable, str(ROOT / "tools/analyse_selftest.py"), "--collect", str(FIXTURE), "--json"],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(j.returncode, 0, j.stderr)
        entries = json.loads(j.stdout)["dumps"]["entries"]
        self.assertEqual(set(entries[0]), {"name", "bytes", "written", "crashed", "process", "pid", "fetched", "when",
                                           "why"})
        self.assertEqual(entries[-1]["written"], "Oct 13 14:09")


class Placement(unittest.TestCase):
    """Folder's boot: the previous boot's kernel.txt last written at 12:00 (its window opens at 11:58), boot.txt's ok
    mark at 12:01 (it closes at 12:01:04), SceShell 0x118FF and PCSG00415 as pid 0x20000 in kernel.txt, whose last
    write at 13:30 and last line's uptime of 90 minutes check the clock (this boot began at 12:00 by it)."""

    WINDOW = "2026-10-20 11:58:00 to 2026-10-20 12:01:04"
    UNCHECKED = ("dated before this boot began, but the Vita's clock is not checked for this boot: a clock set back "
                 "during it would date a crash of this boot so too")

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

    def folder(self, **kw):
        path = self.tmp / f"collect-{len(list(self.tmp.iterdir()))}"
        return Folder(path, **kw)

    def placed(self, f):
        return {e["name"]: (e["when"], e["why"]) for e in f.dumps()["entries"]}

    def test_each_rule(self):
        f = self.folder()
        names = {
            "old app": f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin"),
            "game this boot": f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin"),
            "shell this boot": f.dump(unix("2026-10-20 12:45:00"), 0x118FF, "shell.self"),
            "a stranger after": f.dump(unix("2026-10-21 09:00:00"), 0x118DB, "shell.self"),
            "the previous boot's shell": f.dump(unix("2026-10-20 12:00:30"), 0x11901, "shell.self"),
            "an unknown shell": f.dump(unix("2026-10-20 12:00:40"), 0x12345, "shell.self"),
            "app at the start": f.dump(unix("2026-10-20 12:00:50"), 0x30000, "eboot.bin"),
            "app after the last write": f.dump(unix("2026-10-20 14:00:00"), 0x40000, "eboot.bin"),
            "this boot's game pid, dated before": f.dump(unix("2026-10-19 09:00:00"), 0x20000, "eboot.bin"),
            "this boot's shell pid, dated before": f.dump(unix("2026-10-19 09:30:00"), 0x118FF, "shell.self"),
            "a foreground line's pid, dated before": f.dump(unix("2026-10-19 08:00:00"), 0x3269D, "eboot.bin"),
        }
        p = self.placed(f)
        want = {
            "old app": ("before", "dated before this boot began"),
            "game this boot": ("this boot", "dated after this boot began, and kernel.txt names its pid (0x20000, "
                                            "PCSG00415) this boot"),
            "shell this boot": ("this boot", "dated after this boot began, and its SceShell (pid 0x118FF) is this boot's"),
            # audit N5: an earlier boot's under a clock set back since, or a later boot's the overlay did not record
            "a stranger after": ("unclear", "dated after this boot began, but its SceShell (pid 0x118DB) is not this "
                                            "boot's (kernel.txt registered 0x118FF): an earlier boot's under a clock set "
                                            "back since it crashed, or a later boot's that the overlay did not record "
                                            "(one started with L held)"),
            "the previous boot's shell": ("unclear", f"dated when this boot began ({self.WINDOW}), and its SceShell (pid "
                                                     "0x11901) is not among kernel.txt's registrations: a SceShell that "
                                                     "crashed before the overlay registered it, or an earlier boot's; it "
                                                     "was the previous boot's SceShell pid (kernel.prev.txt), but pids "
                                                     "repeat across boots"),
            "an unknown shell": ("unclear", f"dated when this boot began ({self.WINDOW}), and its SceShell (pid 0x12345) "
                                            "is not among kernel.txt's registrations: a SceShell that crashed before the "
                                            "overlay registered it, or an earlier boot's"),
            "app at the start": ("unclear", "dated when this boot began, between 2026-10-20 11:58:00 and 2026-10-20 "
                                            "12:01:04"),
            "app after the last write": ("this boot", "dated after this boot began; it is also dated after this boot's "
                                                      "newest kernel.txt write: it crashed after it, or the Vita's clock "
                                                      "was set back since"),
            # a pid this boot's kernel.txt named vetoes "before": the pid repeated, or the clock went back during it
            "this boot's game pid, dated before": ("unclear", "dated before this boot began, but kernel.txt names its pid "
                                                              "(0x20000, PCSG00415) this boot: a pid that repeats, or the "
                                                              "clock set back during it"),
            "this boot's shell pid, dated before": ("unclear", "dated before this boot began, but its SceShell (pid "
                                                               "0x118FF) is this boot's: a pid that repeats, or the clock "
                                                               "set back during it"),
            "a foreground line's pid, dated before": ("unclear", "dated before this boot began, but kernel.txt names its "
                                                                 "pid (0x3269D) this boot: a pid that repeats, or the "
                                                                 "clock set back during it"),
        }
        for key, name in names.items():
            with self.subTest(key):
                self.assertEqual(p[name], want[key])
        text = "\n".join(analyse_selftest.render_dumps(f.dumps()))
        self.assertIn("crash dumps: 11 on the card, 3 NEW, from this boot, 1 from before it, 7 not placed;", text)
        self.assertIn(f"    NEW (this boot)  2026-10-20 12:30:00  an app or game (eboot.bin) pid 0x20000, 4,096 B: "
                      f"{names['game this boot']}", text)
        self.assertIn("    not placed       2026-10-20 12:00:50  an app or game (eboot.bin) pid 0x30000", text)
        self.assertIn("    the Vita's clock, checked for this boot: kernel.txt's last uptime line (t=5,400,000 ms) and its "
                      "last write (2026-10-20 13:30 by the clock) put this boot's start at 2026-10-20 12:00:00 to "
                      "2026-10-20 12:00:59", text)
        # oldest first
        self.assertEqual([e["name"] for e in f.dumps()["entries"]][0], names["a foreground line's pid, dated before"])

    def test_every_pid_a_kernel_line_names_vetoes(self):
        # audit N6: the transitions and the input lines name pids too (kernel/lifecycle.c, kernel/main.c, kernel/input.c)
        extra = ("pid 0x44444 gone: back to game pid 0x20000\npid 0x55555 is not a game: back to game pid 0x20000\n"
                 "input block=1 pid=118FF game=66666 raw=00000300 t=5000000 ms\ninput block refused: pid=77777 shell=118FF\n"
                 "ocr worker: second worker thread 0x40010FFF in game pid 0x88888 refused (registered: 0x40010129)\n")
        f = self.folder(kernel=kernel_text(extra=extra))
        dumps = {pid: f.dump(unix("2026-10-19 09:00:00") + i, pid, "eboot.bin")
                 for i, pid in enumerate((0x44444, 0x55555, 0x66666, 0x77777, 0x88888, 0x40010FFF))}
        p = self.placed(f)
        for pid in (0x44444, 0x55555, 0x66666, 0x77777, 0x88888):
            with self.subTest(pid=hex(pid)):
                self.assertEqual(p[dumps[pid]][0], "unclear")
                self.assertIn(f"kernel.txt names its pid (0x{pid:X}) this boot", p[dumps[pid]][1])
        self.assertEqual(p[dumps[0x40010FFF]], ("before", "dated before this boot began"))  # a thread, not a pid

    # ---- the clock (audit B1): "before" from a date only with the clock checked for this boot

    def test_audit_case_2_a_set_back_of_minutes_during_the_boot(self):
        # set back 20 minutes 10 minutes into the boot, never put right; the Rubi app (whose pid the kernel never logs)
        # crashes after it, dated 11:52; kernel.txt's last write at 13:30 true is dated 13:10
        f = self.folder()
        f.times["ux0:/data/VitaJPOverlay"]["kernel.txt"] = unix("2026-10-20 13:10:40")
        f.times["ux0:/data/VitaJPOverlay"]["status.txt"] = unix("2026-10-20 12:05:00")
        f.save()
        app = f.dump(unix("2026-10-20 11:52:00"), 0x50000, "eboot.bin")
        game = f.dump(unix("2026-10-20 11:53:00"), 0x20000, "eboot.bin")
        why = ("kernel.txt's last uptime line (t=5,400,000 ms) and its last write (2026-10-20 13:10 by the clock) put this "
               "boot's start at 2026-10-20 11:40:00 to 2026-10-20 11:40:59, before 2026-10-20 11:58:00: the Vita's clock "
               "was set back during this boot, or between the boots")
        self.assertEqual(f.dumps()["boot"], {"unknown": why})
        self.assertEqual(self.placed(f), {app: ("unclear", why), game: ("unclear", why)})
        self.assertIn(f"this boot's start is unknown: {why}", "\n".join(analyse_selftest.render_dumps(f.dumps())))

    def test_a_start_whose_minute_straddles_the_window(self):
        # kernel.txt last written in the minute 13:27, 89.5 minutes in: by the clock this boot began between 11:57:30 and
        # 11:58:29. Unchanged, the clock puts it no earlier than a minute before kernel.prev.txt's 12:00 (LIST's minutes),
        # so any second before the window (11:58) is a set-back
        f = self.folder(kernel=kernel_text(last_ms=5_370_000))
        f.times["ux0:/data/VitaJPOverlay"]["kernel.txt"] = unix("2026-10-20 13:27:10")
        f.save()
        old = f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
        self.assertEqual(f.dumps()["boot"], {"unknown": "kernel.txt's last uptime line (t=5,370,000 ms) and its last write "
                                                        "(2026-10-20 13:27 by the clock) put this boot's start at "
                                                        "2026-10-20 11:57:30 to 2026-10-20 11:58:29, before 2026-10-20 "
                                                        "11:58:00: the Vita's clock was set back during this boot, or "
                                                        "between the boots"})
        self.assertEqual(self.placed(f)[old][0], "unclear")

    def test_audit_case_3_a_set_back_of_hours_early_in_a_long_session(self):
        # set back 2 hours 20 minutes into a 4-hour session; VitaShell crashes after it, dated 10:40
        f = self.folder(kernel=kernel_text(last_ms=14_400_000))
        f.times["ux0:/data/VitaJPOverlay"]["kernel.txt"] = unix("2026-10-20 14:00:40")
        f.times["ux0:/data/VitaJPOverlay"]["status.txt"] = unix("2026-10-20 12:15:00")
        f.save()
        shell = f.dump(unix("2026-10-20 10:40:00"), 0x60000, "eboot.bin")
        self.assertEqual(self.placed(f)[shell][0], "unclear")
        self.assertIn("put this boot's start at 2026-10-20 10:00:00 to 2026-10-20 10:00:59, before 2026-10-20 11:58:00",
                      self.placed(f)[shell][1])

    def test_audit_case_4_a_set_back_after_kernel_txts_cap(self):
        # kernel.txt and status.txt both at their caps; then set back 3 days, and a game started after the cap crashes
        f = self.folder(kernel=KERNEL + "x" * (analyse_selftest.KERNEL_TXT_CAP - len(KERNEL)))
        f.times["ux0:/data/VitaJPOverlay"]["kernel.txt"] = unix("2026-10-20 12:50:00")
        f.times["ux0:/data/VitaJPOverlay"]["status.txt"] = unix("2026-10-20 12:05:00")
        f.save()
        game = f.dump(unix("2026-10-17 13:10:00"), 0x70000, "eboot.bin")
        self.assertEqual(self.placed(f)[game], ("unclear", self.UNCHECKED))
        self.assertEqual(f.dumps()["boot"]["clock"], {"state": "unchecked", "why": "kernel.txt reached its cap, so its "
                                                                                  "last write is not this boot's latest"})
        self.assertIn("    the Vita's clock, NOT checked for this boot: kernel.txt reached its cap, so its last write is not "
                      "this boot's latest; no dump is placed before this boot by its date",
                      "\n".join(analyse_selftest.render_dumps(f.dumps())))

    def test_audit_case_5_an_estimated_window(self):
        # no kernel.prev.txt: the window is an estimate, and a late ok mark (two SceShells crashed before they
        # registered) leaves it too short; nothing is placed before it
        f = self.folder(kernel_prev=None)
        for n in ("kernel.prev.txt", "status.prev.txt"):
            del f.times["ux0:/data/VitaJPOverlay"][n]
        f.times["ur0:/data/VitaJPOverlay"]["boot.txt"] = unix("2026-10-20 12:04:10")
        f.save()
        first = f.dump(unix("2026-10-20 12:00:08"), 0x118A0, "shell.self")
        second = f.dump(unix("2026-10-20 12:01:30"), 0x118A2, "shell.self")
        p = self.placed(f)
        self.assertEqual(p[first], ("unclear", self.UNCHECKED))
        self.assertEqual(p[second][0], "unclear")
        self.assertEqual(f.dumps()["boot"]["clock"], {"state": "unchecked",
                                                      "why": "the window is an estimate (no kernel.prev.txt)"})

    def test_audit_case_6_a_later_boot_the_overlay_did_not_record(self):
        # started with L held: no plugin, no boot.txt write; its SceShell dump is newer than "this boot", not older
        f = self.folder()
        later = f.dump(unix("2026-10-20 18:00:00"), 0x118AA, "shell.self")
        self.assertEqual(self.placed(f)[later][0], "unclear")
        self.assertIn("or a later boot's that the overlay did not record (one started with L held)", self.placed(f)[later][1])

    def test_audit_case_7_a_set_back_between_the_boots_and_a_late_ok_mark(self):
        # the previous boot's last kernel.txt write at 10:00 by the old clock; this boot began at 09:57 by the new one, a
        # SceShell crashed at 09:57:05 before it registered, the ok mark came at 10:00:10; kernel.txt's last write at
        # 11:00 by the clock, 63 minutes in
        f = self.folder(kernel=kernel_text(last_ms=3_780_000))
        f.times["ux0:/data/VitaJPOverlay"].update({
            "kernel.prev.txt": unix("2026-10-20 10:00:00"), "status.prev.txt": unix("2026-10-20 10:00:00"),
            "kernel.txt": unix("2026-10-20 11:00:00"), "status.txt": unix("2026-10-20 10:30:00")})
        f.times["ur0:/data/VitaJPOverlay"]["boot.txt"] = unix("2026-10-20 10:00:10")
        f.save()
        shell = f.dump(unix("2026-10-20 09:57:05"), 0x118A0, "shell.self")
        self.assertEqual(self.placed(f)[shell][0], "unclear")
        self.assertIn("put this boot's start at 2026-10-20 09:57:00 to 2026-10-20 09:57:59, before 2026-10-20 09:58:00",
                      self.placed(f)[shell][1])

    def test_the_limit_a_set_back_after_kernel_txts_last_write(self):
        # the documented limit (docs/self-test.md#crash-dumps): kernel.txt's last write saw the clock unchanged; a
        # set-back after it, before the collect, dates a later crash of this boot (an app whose pid the kernel never
        # logs) before the boot, and it is placed before it. Archiving keeps the collect's copy.
        f = self.folder()
        app = f.dump(unix("2026-10-20 11:30:00"), 0x50000, "eboot.bin")
        self.assertEqual(f.dumps()["boot"]["clock"]["state"], "checked")
        self.assertEqual(self.placed(f)[app], ("before", "dated before this boot began"))

    def test_delta_audit_b2_a_raced_game_exit_and_a_lone_one(self):
        # the clock set back 1 hour 5 minutes into the boot, after status.txt's last write; the Rubi app (whose pid the
        # kernel never names) crashes at 12:10 true, dated 11:10. The game's suspend at 30 minutes has its game exit
        # logged first, and the Rubi app's exit at 13:30 true, dated 12:30, ends kernel.txt with a lone game exit
        f = self.folder(kernel=KERNEL_HEAD + "game exit\ngame exit\nforeground 0x20000 -> 0x0 (suspend) t=1800000 ms\n"
                                             "game exit\n")
        f.times["ux0:/data/VitaJPOverlay"]["kernel.txt"] = unix("2026-10-20 12:30:20")
        f.times["ux0:/data/VitaJPOverlay"]["status.txt"] = unix("2026-10-20 12:04:00")
        f.save()
        app = f.dump(unix("2026-10-20 11:10:00"), 0x50000, "eboot.bin")
        self.assertEqual(f.dumps()["boot"]["clock"]["state"], "unchecked")
        self.assertEqual(self.placed(f)[app], ("unclear", self.UNCHECKED))

    def test_the_forward_tolerance(self):
        # with the clock unchanged the start lies in the window, at most LIST's minute after its close; 2 minutes more
        # are allowed. The window closes at 12:01:04; kernel.txt's last write is in the minute 13:30
        cases = {  # uptime of the last line -> (this boot's start by the clock, the clock checked)
            5_340_000: ("2026-10-20 12:01:00 to 2026-10-20 12:01:59", True),  # a minute after the close
            5_249_000: ("2026-10-20 12:02:31 to 2026-10-20 12:03:30", False),  # its latest second past 12:03:04
            5_160_000: ("2026-10-20 12:04:00 to 2026-10-20 12:04:59", False),  # 3 minutes after
        }
        for uptime, (start, checked) in cases.items():
            with self.subTest(uptime=uptime):
                f = self.folder(kernel=kernel_text(last_ms=uptime))
                old = f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
                clock = f.dumps()["boot"]["clock"]
                self.assertIn(f"put this boot's start at {start}", clock["why"])
                self.assertEqual(clock["state"], "checked" if checked else "unchecked")
                self.assertEqual(self.placed(f)[old][0], "before" if checked else "unclear")

    def test_a_clock_gone_forward(self):
        # kernel.txt's last write gives a start an hour after the window: the clock went forward (or the Vita slept),
        # which may hide a set-back before it
        f = self.folder(kernel=kernel_text(last_ms=1_800_000))
        old = f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
        self.assertEqual(self.placed(f)[old], ("unclear", self.UNCHECKED))
        self.assertEqual(f.dumps()["boot"]["clock"]["state"], "unchecked")
        self.assertIn("put this boot's start at 2026-10-20 13:00:00 to 2026-10-20 13:00:59, after 2026-10-20 12:01:04: the "
                      "Vita's clock went forward during this boot, or the Vita slept", f.dumps()["boot"]["clock"]["why"])

    def test_the_last_lines_uptime(self):
        # only kernel.txt's last line gives its last write's uptime; "game exit" may follow the foreground line of its
        # own exit or suspend, and nothing else
        cases = {
            "the last line has none": (KERNEL_HEAD + "region settled: scene 1 (0 animated cells masked)\n", "unchecked",
                                       "kernel.txt's last line carries no uptime (t=N ms)"),
            "game exit after a game line": (KERNEL_HEAD + "game exit\n", "unchecked",
                                            "kernel.txt's last line carries no uptime (t=N ms)"),
            "game exit after its suspend": (kernel_text(), "checked", None),
            "game exit after a resume": (KERNEL_HEAD + "foreground 0x0 -> 0x20000 (resume) t=5400000 ms\ngame exit\n",
                                         "unchecked", "kernel.txt's last line carries no uptime (t=N ms)"),
            # other threads' lines between the previous locked line ("game pid ... active") and the suspend
            "game exit after its suspend, other lines between": (
                kernel_text(extra="region settled: scene 1 (0 animated cells masked)\nocr job 1: first pass in (135 "
                                  "rows, frame 960x544 fmt 00000000 pitch 1024), scene 1\n"), "checked", None),
            # a suspend, a resume and a suspend: the first suspend's game exit lies before a locked line
            "game exit after its suspend, an earlier suspend's before a resume": (
                kernel_text(extra="foreground 0x20000 -> 0x0 (suspend) t=1000000 ms\ngame exit\n"
                                  "foreground 0x0 -> 0x20000 (resume) t=1100000 ms\n"
                                  "game pid 0x20000 active t=1100100 ms\n"), "checked", None),
            # the delta audit's B2: the suspend's game exit logged first (test.3's lines 104-105), then a lone one,
            # another app's exit, at any later time
            "game exit, the suspend, a lone game exit": (
                KERNEL_HEAD + "game exit\nforeground 0x20000 -> 0x0 (suspend) t=5400000 ms\ngame exit\n", "unchecked",
                "kernel.txt ends with a game exit that may be a later app's: one is logged before its last foreground "
                "line too, which the worker can log first"),
            "game exit, another thread's line, the suspend, a lone game exit": (
                KERNEL_HEAD + "game exit\ninput blocked hook=2 pid=20000 game=20000 read=00000300 write=00000000 "
                "buttons=00000300 out=00000000 LR=1/1 L1R1=0/0 raw=00000300\n"
                "foreground 0x20000 -> 0x0 (suspend) t=5400000 ms\ngame exit\n", "unchecked",
                "kernel.txt ends with a game exit that may be a later app's: one is logged before its last foreground "
                "line too, which the worker can log first"),
            "game exit, then the suspend last": (
                KERNEL_HEAD + "game exit\nforeground 0x20000 -> 0x0 (suspend) t=5400000 ms\n", "checked", None),
            "the suspend's game exit, then a lone one": (kernel_text() + "game exit\n", "unchecked",
                                                         "kernel.txt's last line carries no uptime (t=N ms)"),
            # audit-c N-1: lines the kernel logs without g.game_lock never end the scan, an uptime included: the debug
            # build's input block= (a Shell syscall), shell registered (g.lock only), ocr worker registered (a game's)
            "game exit, the debug build's input block=, the suspend, a lone game exit": (
                KERNEL_HEAD + "game exit\ninput block=0 pid=118FF game=0 raw=00000000 t=5390000 ms\n"
                "foreground 0x20000 -> 0x0 (suspend) t=5400000 ms\ngame exit\n", "unchecked",
                "kernel.txt ends with a game exit that may be a later app's: one is logged before its last foreground "
                "line too, which the worker can log first"),
            "game exit, a Shell registration, the suspend, a lone game exit": (
                KERNEL_HEAD + "game exit\nshell registered: pid 0x118AA (was 0x118FF)\n"
                "foreground 0x20000 -> 0x0 (suspend) t=5400000 ms\ngame exit\n", "unchecked",
                "kernel.txt ends with a game exit that may be a later app's: one is logged before its last foreground "
                "line too, which the worker can log first"),
            "game exit, an OCR worker registration, the suspend, a lone game exit": (
                KERNEL_HEAD + "game exit\nocr worker registered: game pid 0x20000 module 0x30000\n"
                "foreground 0x20000 -> 0x0 (suspend) t=5400000 ms\ngame exit\n", "unchecked",
                "kernel.txt ends with a game exit that may be a later app's: one is logged before its last foreground "
                "line too, which the worker can log first"),
            # audit-c N-1: a startup over a confirmed game, or the Shell's verdict, raises no game exit of its own: a game
            # exit after either is a later, lone one (kernel/foreground.c)
            "a startup over the game, then a lone game exit": (
                KERNEL_HEAD + "foreground 0x20000 -> 0x30000 (startup) t=5400000 ms\ngame exit\n", "unchecked",
                "kernel.txt's last line carries no uptime (t=N ms)"),
            "the Shell's verdict, then a lone game exit": (
                KERNEL_HEAD + "foreground 0x30000 -> 0x20000 (shell) t=5400000 ms\ngame exit\n", "unchecked",
                "kernel.txt's last line carries no uptime (t=N ms)"),
            "uptimes that go back": (kernel_text(last_ms=50_000) + "foreground 0x0 -> 0x20000 (resume) t=40000 ms\n",
                                     "unchecked", "kernel.txt's uptimes go backwards"),
        }
        for key, (text, state, why) in cases.items():
            with self.subTest(key):
                f = self.folder(kernel=text)
                old = f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
                clock = f.dumps()["boot"]["clock"]
                self.assertEqual(clock["state"], state)
                if why:
                    self.assertEqual(clock["why"], why)
                self.assertEqual(self.placed(f)[old][0], "before" if state == "checked" else "unclear")

    def test_a_safe_boots_sceshell(self):
        # a safe boot registers no SceShell (its Shell plugin unloads itself): a SceShell dump after the window is this
        # boot's, never a stranger
        f = self.folder(kernel=kernel_text().replace("shell registered: pid 0x118FF (was 0x0)\n", ""))
        shell = f.dump(unix("2026-10-20 12:40:00"), 0x118FF, "shell.self")
        self.assertEqual(self.placed(f)[shell], ("this boot", "dated after this boot began"))

    def test_a_sceshell_restart_this_boot(self):
        # audit-review B1: SceShell 0x118FF crashed at 13:00 and restarted as 0x118AA. The restarted Shell plugin renamed
        # status.txt to status.prev.txt then (shell/log_vita.c), and the kernel wrote boot.txt with fault=1; neither may
        # move the boot's start: the game's 12:30 dump is never "before"
        f = self.folder(boot="vjoboot1 seq=1 mode=0 stage=4 inert=0 fault=1 failures=0 api=22 prev=0/0/0/0\n",
                        kernel=kernel_text(extra="shell registered: pid 0x118AA (was 0x118FF)\n"))
        f.times["ur0:/data/VitaJPOverlay"]["boot.txt"] = unix("2026-10-20 13:00:20")
        f.times["ux0:/data/VitaJPOverlay"]["status.prev.txt"] = unix("2026-10-20 13:00:10")
        f.save()
        game = f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin")
        first = f.dump(unix("2026-10-20 13:00:05"), 0x118FF, "shell.self")
        again = f.dump(unix("2026-10-20 13:02:00"), 0x118CC, "shell.self")  # the restarted one crashed before registering
        d = f.dumps()
        self.assertEqual((d["boot"]["from"], d["boot"]["to"]), ("2026-10-20 11:58:00", "2026-10-20 13:00:59"))
        p = self.placed(f)
        self.assertEqual(p[game], ("unclear", "dated when this boot began, between 2026-10-20 11:58:00 and 2026-10-20 "
                                              "13:00:59; kernel.txt names its pid (0x20000, PCSG00415) this boot, but "
                                              "pids repeat across boots"))
        self.assertEqual(p[first], ("this boot", "its SceShell (pid 0x118FF) is this boot's, and boot.txt records "
                                                 "SceShell's exit (fault=1)"))
        self.assertEqual(p[again][0], "unclear")
        self.assertIn("its SceShell (pid 0x118CC) is not this boot's (kernel.txt registered 0x118AA, 0x118FF)", p[again][1])
        self.assertNotIn("before", [w for w, _ in p.values()])

    def test_the_clock_set_back_during_this_boot(self):
        # audit-review S3: kernel.txt last written on Oct 17 by a clock set back after this boot began on Oct 20: no date
        # of this boot can be trusted
        f = self.folder()
        f.times["ux0:/data/VitaJPOverlay"]["kernel.txt"] = unix("2026-10-17 13:30:00")
        f.save()
        game = f.dump(unix("2026-10-17 13:05:00"), 0x20000, "eboot.bin")
        old = f.dump(unix("2026-10-11 13:57:17"), 0x72FC5, "eboot.bin")
        d = f.dumps()
        why = ("this boot's kernel.txt was last written 2026-10-17 13:30, before this boot began (2026-10-20 11:58:00 "
               "or later): the Vita's clock was set back during this boot")
        self.assertEqual(d["boot"], {"unknown": why})
        self.assertEqual(self.placed(f), {game: ("unclear", why), old: ("unclear", why)})

    def test_the_clock_set_back_after_kernel_txt_reached_its_cap(self):
        # kernel.txt stopped at its cap before the clock went back; status.txt, written after, shows it
        f = self.folder(kernel=KERNEL + "x" * (analyse_selftest.KERNEL_TXT_CAP - len(KERNEL)))
        f.times["ux0:/data/VitaJPOverlay"]["status.txt"] = unix("2026-10-17 14:00:00")
        f.save()
        game = f.dump(unix("2026-10-17 13:05:00"), 0x50000, "eboot.bin")
        self.assertEqual(self.placed(f)[game], ("unclear", "this boot's status.txt was last written 2026-10-17 14:00, "
                                                           "before this boot began (2026-10-20 11:58:00 or later): the "
                                                           "Vita's clock was set back during this boot"))

    def test_a_kernel_txt_the_ring_cut(self):
        # kernel.txt that does not start with boot_init's line: the 4 KiB ring dropped its oldest bytes before the first
        # write, a registration may be gone: no pid is used, and the clock is not checked
        f = self.folder(kernel="0x0 (was 0x0)\n" + KERNEL.split("\n", 4)[4])
        shell = f.dump(unix("2026-10-20 13:00:00"), 0x118DB, "shell.self")
        old = f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
        d = f.dumps()
        self.assertEqual(self.placed(f), {shell: ("this boot", "dated after this boot began"),
                                          old: ("unclear", self.UNCHECKED)})
        text = "\n".join(analyse_selftest.render_dumps(d))
        self.assertIn("    no SceShell or process pid is used: kernel.txt does not start with the boot's first line (lines "
                      "may be missing)", text)
        self.assertIn("the Vita's clock, NOT checked for this boot: kernel.txt does not start with the boot's first line",
                      text)

    def test_a_clock_set_back_a_few_minutes_before_the_reboot(self):
        # re-review: kernel.prev.txt 12:00, boot.txt's ok mark 11:58 (a clock put back 3 minutes before the reboot); the
        # window would have closed at 11:58:04 and this boot's 11:57:30 SceShell dump looked "before"
        f = self.folder()
        f.times["ux0:/data/VitaJPOverlay"]["kernel.prev.txt"] = unix("2026-10-20 12:00:30")
        f.times["ur0:/data/VitaJPOverlay"]["boot.txt"] = unix("2026-10-20 11:58:20")
        f.save()
        shell = f.dump(unix("2026-10-20 11:57:30"), 0x118AB, "shell.self")
        self.assertEqual(self.placed(f)[shell], ("unclear", "boot.txt's last write (Oct 20 11:58) is not 55 s or more "
                                                            "after kernel.prev.txt's (Oct 20 12:00): the Vita's clock was "
                                                            "set back between the two boots"))

    def test_a_ring_cut_kernel_txt_still_shows_a_clock_set_back(self):
        # re-review: kernel.txt's pids are not used (the ring cut it), but its time still says the clock went back
        f = self.folder(kernel="0x0 (was 0x0)\n" + KERNEL.split("\n", 4)[4])
        f.times["ux0:/data/VitaJPOverlay"]["kernel.txt"] = unix("2026-10-17 13:30:00")
        f.save()
        game = f.dump(unix("2026-10-17 13:05:00"), 0x20000, "eboot.bin")
        self.assertEqual(self.placed(f)[game][0], "unclear")
        self.assertIn("the Vita's clock was set back during this boot", self.placed(f)[game][1])

    def test_another_boots_kernel_txt(self):
        # boot.txt is boot 2's, kernel.txt boot 1's (boot 2 never got to write it): neither its pids nor its clock check
        # are this boot's
        f = self.folder(boot=BOOT_OK.replace("seq=1", "seq=2"))
        game = f.dump(unix("2026-10-19 09:00:00"), 0x20000, "eboot.bin")
        self.assertEqual(self.placed(f)[game], ("unclear", self.UNCHECKED))
        self.assertEqual(f.dumps()["pids_note"], "no SceShell or process pid is used: kernel.txt is boot 1's, boot.txt "
                                                 "boot 2's")

    def test_ux0_data_could_not_be_listed(self):
        f = self.folder()
        (f.path / "listings/ux0_data.txt").unlink()
        record = json.loads((f.path / "COLLECT.json").read_text())
        record["errors"] = ["ux0:/data/: LIST failed: 550 injected failure"]
        (f.path / "COLLECT.json").write_text(json.dumps(record))
        d = analyse_selftest.summarise_dumps(f.path, record)
        self.assertEqual(analyse_selftest.render_dumps(d), ["  crash dumps: not known: ux0:data could not be listed "
                                                            "(ux0:/data/: LIST failed: 550 injected failure)"])
        r = subprocess.run([sys.executable, str(ROOT / "tools/analyse_selftest.py"), "--collect", str(f.path)],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("crash dump on the card", r.stdout)
        self.assertNotIn("crash dumps on the card", r.stdout)

    def test_a_kernel_txt_at_its_cap(self):
        # kernel.txt stops at 256 KB (kernel/main.c KFILE_CAP): a SceShell restarted after that is not registered in it,
        # and its last write is not this boot's latest
        kernel = KERNEL + "x" * (analyse_selftest.KERNEL_TXT_CAP - len(KERNEL))
        f = self.folder(kernel=kernel)
        shell = f.dump(unix("2026-10-20 13:00:00"), 0x118DB, "shell.self")
        old = f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
        p = self.placed(f)
        self.assertEqual(p[shell][0], "unclear")
        self.assertIn("its SceShell (pid 0x118DB) is not this boot's", p[shell][1])
        self.assertEqual(p[old], ("unclear", self.UNCHECKED))

    def test_a_dump_the_collect_could_not_fetch_is_listed(self):
        f = self.folder()
        name = f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin", fetched=False)
        e = f.dumps()["entries"][0]
        self.assertEqual((e["name"], e["fetched"], e["when"]), (name, False, "this boot"))
        self.assertIn(", 4,096 B, NOT fetched: ", "\n".join(analyse_selftest.render_dumps(f.dumps())))

    def test_a_fetched_dump_without_a_listing(self):
        # an older collect that has the file but no listing of ux0:data: dated by its name
        f = self.folder()
        f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin", listed=False)
        e = f.dumps()["entries"][0]
        self.assertEqual((e["written"], e["when"], f.dumps()["year_from"]), (None, "before",
                                                                             "this computer's clock at the collect "
                                                                             "(no dump names the Vita's)"))

    def test_a_name_without_a_time_is_dated_by_its_listing(self):
        f = self.folder()
        f.times["ux0:/data"]["psp2core-crash.psp2dmp"] = (unix("2026-10-20 13:00:00"), 100)
        f.save()
        e = f.dumps()["entries"][0]
        self.assertEqual((e["crashed"], e["written"], e["pid"], e["process"], e["when"]),
                         (None, "Oct 20 13:00", None, "?", "this boot"))

    def test_boots_whose_start_is_unknown(self):
        cases = {
            "the installer's reset": dict(boot=""),
            "no record": dict(boot=None),
            "no kernel.prev.txt and no ok mark": dict(
                boot="vjoboot1 seq=1 mode=0 stage=3 inert=0 fault=0 failures=0 api=22 prev=0/0/0/0\n", kernel_prev=None),
        }
        whys = {"the installer's reset": "boot.txt is empty: the installer reset it, and no boot has written it since",
                "no record": "the collect has no LIST time of ur0:data/VitaJPOverlay/boot.txt, the boot guard's record",
                "no kernel.prev.txt and no ok mark": "there is no kernel.prev.txt, and boot.txt's last write is not its "
                                                     "ok mark (stage=3, fault=0, inert=0)"}
        for key, kw in cases.items():
            with self.subTest(key):
                f = self.folder(**kw)
                if kw.get("boot", BOOT_OK) is None:
                    del f.times["ur0:/data/VitaJPOverlay"]["boot.txt"]
                if kw.get("kernel_prev", KERNEL_PREV) is None:
                    del f.times["ux0:/data/VitaJPOverlay"]["kernel.prev.txt"]
                f.save()
                app = f.dump(unix("2026-10-19 09:00:00"), 0x72FC5, "eboot.bin")
                shell = f.dump(unix("2026-10-21 09:00:00"), 0x118DB, "shell.self")
                p = self.placed(f)
                self.assertEqual(p[app], ("unclear", whys[key]))
                self.assertEqual(p[shell], ("unclear", whys[key]))  # no pid places a dump without a boot to place it in
                self.assertIn(f"this boot's start is unknown: {whys[key]}",
                              "\n".join(analyse_selftest.render_dumps(f.dumps())))

    def test_a_clock_set_back_between_the_boots(self):
        f = self.folder()
        f.times["ux0:/data/VitaJPOverlay"]["kernel.prev.txt"] = unix("2026-10-23 08:00:00")
        f.save()
        app = f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin")
        self.assertEqual(self.placed(f)[app], ("unclear", "boot.txt's last write (Oct 20 12:01) is not 55 s or more after "
                                                          "kernel.prev.txt's (Oct 23 08:00): the Vita's clock was set back "
                                                          "between the two boots"))

    def test_without_kernel_prev_txt_the_start_is_an_estimate(self):
        f = self.folder(kernel_prev=None)
        for n in ("kernel.prev.txt", "status.prev.txt"):
            del f.times["ux0:/data/VitaJPOverlay"][n]
        f.save()
        old = f.dump(unix("2026-10-20 11:30:00"), 0x72FC5, "eboot.bin")
        d = f.dumps()
        self.assertTrue(d["boot"]["estimate"])
        self.assertEqual((d["boot"]["from"], d["boot"]["to"]), ("2026-10-20 11:58:04", "2026-10-20 12:01:04"))
        self.assertEqual(d["boot"]["how"], "an estimate, as there is no kernel.prev.txt: 55 s to 3 minutes before "
                                           "boot.txt's ok mark")
        self.assertEqual(self.placed(f)[old], ("unclear", self.UNCHECKED))  # audit N4: no "before" from an estimate

    def test_status_prev_txt_is_not_the_previous_boots(self):
        # status.txt is rotated at every Shell start, a SceShell restart included: its time never moves the boot's start
        f = self.folder()
        f.times["ux0:/data/VitaJPOverlay"]["status.prev.txt"] = unix("2026-10-20 12:50:00")
        f.save()
        f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin")
        self.assertEqual(f.dumps()["boot"]["from"], "2026-10-20 11:58:00")

    def test_a_record_not_at_its_ok_mark(self):
        # a boot that never reached ok: boot.txt's last write was an earlier mark, the start before it
        f = self.folder(boot="vjoboot1 seq=1 mode=0 stage=3 inert=0 fault=0 failures=0 api=22 prev=0/4/0/0\n")
        f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin")
        d = f.dumps()
        self.assertEqual(d["boot"]["to"], "2026-10-20 12:01:59")
        self.assertIn("before boot.txt's last write", d["boot"]["how"])

    def test_the_year_of_a_listed_time(self):
        # LIST leaves the year out of a time of the Vita's current year: a dump listed so gives it; a dump of another
        # year is listed with its year
        f = self.folder()
        f.dump(unix("2025-12-31 23:00:00"), 0x72FC5, "eboot.bin")
        self.assertEqual(f.dumps()["year_from"], "this computer's clock at the collect (no dump names the Vita's)")
        f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin")
        d = f.dumps()
        self.assertEqual((d["year"], d["year_from"]), (2026, "the crash dumps' names"))
        self.assertEqual([e["written"] for e in d["entries"]], ["Dec 31 2025", "Oct 20 12:30"])
        self.assertEqual([e["when"] for e in d["entries"]], ["before", "this boot"])

    def test_no_dump(self):
        f = self.folder()
        self.assertIsNone(f.dumps())
        self.assertEqual(analyse_selftest.render_dumps(None), ["  crash dumps: none"])


class Plan(unittest.TestCase):
    """archive_plan: only a dump the collect itself places before its boot, with a system dump name and an intact copy;
    an analysis.json is never read."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.f = Folder(Path(tmp.name) / "c")

    def test_what_is_archived_and_what_stays(self):
        old = self.f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
        changed = self.f.dump(unix("2026-10-18 10:00:00"), 0x72FC6, "eboot.bin")
        unfetched = self.f.dump(unix("2026-10-17 10:00:00"), 0x72FC7, "eboot.bin", fetched=False)
        new = self.f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin")
        unclear = self.f.dump(unix("2026-10-20 12:00:50"), 0x30000, "eboot.bin")
        flipped = self.f.dump(unix("2026-10-18 11:00:00"), 0x72FC8, "eboot.bin")
        (self.f.path / "ux0/data" / changed).write_bytes(b"not what was fetched")
        copy = self.f.path / "ux0/data" / flipped  # the same size, one byte changed: its SHA-256 differs
        data = bytearray(copy.read_bytes())
        data[100] ^= 1
        copy.write_bytes(bytes(data))
        archive, keep, problem = collect_vita.archive_plan(self.f.path)
        self.assertIsNone(problem)
        self.assertEqual([d["name"] for d in archive], [old])
        self.assertEqual(archive[0]["sha256"], self.f.files[f"ux0:/data/{old}"]["sha256"])
        differs = "this collect's copy is not the file it fetched (its size or SHA-256 differs)"
        self.assertEqual(dict(keep), {changed: differs, flipped: differs, unfetched: "this collect has no copy of it",
                                      new: "from this boot", unclear: "not placed before this boot"})

    def test_this_boots_dump_the_collect_could_not_fetch(self):
        new = self.f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin", fetched=False)
        self.assertEqual(collect_vita.archive_plan(self.f.path),
                         ([], [(new, "from this boot; this collect could not fetch it")], None))

    def test_names_it_never_deletes(self):
        good = self.f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
        old = unix("2026-10-19 11:00:00")
        for bad in ("psp2core-1-0x1-a b.psp2dmp", "x.psp2dmp", "..\\psp2core-1-0x1-eboot.bin.psp2dmp",
                    "psp2core-1-0x1-" + "e" * 70 + ".psp2dmp"):
            self.f.times["ux0:/data"][bad] = (old, 4096)
        self.f.save()
        archive, keep, _ = collect_vita.archive_plan(self.f.path)
        self.assertEqual([d["name"] for d in archive], [good])
        self.assertEqual(sorted(keep), sorted((bad, "not a crash dump name the archive deletes") for bad in (
            "psp2core-1-0x1-a b.psp2dmp", "x.psp2dmp", "..\\psp2core-1-0x1-eboot.bin.psp2dmp",
            "psp2core-1-0x1-" + "e" * 70 + ".psp2dmp")))

    def test_an_edited_analysis_json_changes_nothing(self):
        new = self.f.dump(unix("2026-10-20 12:30:00"), 0x20000, "eboot.bin")
        (self.f.path / "analysis.json").write_text(json.dumps({"dumps": {"entries": [
            {"name": new, "bytes": 4096, "when": "before", "crashed": "x", "process": "x"}]}}))
        self.assertEqual(collect_vita.archive_plan(self.f.path), ([], [(new, "from this boot")], None))

    def test_a_dump_the_listing_lacks_stays(self):
        # fetched but not in the collect's listing of ux0:data: no LIST time to compare the Vita's file with
        old = self.f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin", listed=False)
        self.assertEqual(collect_vita.archive_plan(self.f.path),
                         ([], [(old, "the collect's listing of ux0:data does not have it")], None))

    def test_a_copy_shorter_than_the_listing_stays(self):
        # the collect's copy is the file it recorded, but shorter than the LIST it fetched it by
        old = self.f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
        t, size = self.f.times["ux0:/data"][old]
        self.f.times["ux0:/data"][old] = (t, size + 904)
        self.f.save()
        self.assertEqual(collect_vita.archive_plan(self.f.path),
                         ([], [(old, "the collect fetched 4,096 B of its 5000 B")], None))

    def test_an_older_collect_without_dates_compares_its_listing(self):
        # audit N1: a collect made before COLLECT.json carried LIST times; the date is the one its own listing has
        old = self.f.dump(unix("2026-10-19 10:00:00"), 0x72FC5, "eboot.bin")
        archive, keep, problem = collect_vita.archive_plan(self.f.path)
        self.assertEqual([(d["name"], d["date"]) for d in archive], [(old, "Oct 19 10:00")])

    def test_no_collect_record(self):
        (self.f.path / "COLLECT.json").unlink()
        self.assertIn("has no readable COLLECT.json", collect_vita.archive_plan(self.f.path)[2])
        (self.f.path / "COLLECT.json").write_text(json.dumps({"files": {}}))
        self.assertEqual(collect_vita.archive_plan(self.f.path), ([], [], None))  # no dump: nothing to do


class Archive(unittest.TestCase):
    """install_ftp.sh --collect, then its offer and --archive-dumps, against the FTP mock over a device tree whose
    files carry the Vita's clock in their times."""

    VITA_NOW = unix("2026-10-20 14:00:00")

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.package = self.root / "package"
        (self.package / "tools").mkdir(parents=True)
        for name in ("install_ftp.sh", "collect_vita.py", "analyse_selftest.py", "analyse_timeline.py",
                     "model_tiers.json"):
            shutil.copyfile(ROOT / "tools" / name, self.package / "tools" / name)
        stage_profile_builder(self.package)
        self.device = self.root / "device"
        data = self.device / "ux0/data/VitaJPOverlay"
        data.mkdir(parents=True)
        (self.device / "ur0/data/VitaJPOverlay").mkdir(parents=True)
        (self.device / "ur0/tai").mkdir(parents=True)
        (self.device / "ur0/tai/config.txt").write_text("*KERNEL\nur0:tai/VitaJPOverlay_Kernel.skprx\n")
        self.put("ux0/data/VitaJPOverlay/kernel.prev.txt", KERNEL_PREV, "2026-10-20 12:00:20")
        self.put("ux0/data/VitaJPOverlay/status.prev.txt", "bootstrap: shell API 22 entered\n", "2026-10-20 12:00:05")
        self.put("ux0/data/VitaJPOverlay/kernel.txt", KERNEL, "2026-10-20 13:30:40")
        self.put("ux0/data/VitaJPOverlay/status.txt", "bootstrap: shell API 22 entered\n", "2026-10-20 12:40:00")
        self.put("ur0/data/VitaJPOverlay/boot.txt", BOOT_OK, "2026-10-20 12:01:10")
        self.put("ux0/data/Rubi.vpk", "vpk", "2026-10-19 09:00:00")
        self.old = [self.put_dump("2026-10-11 13:57:17", 0x72FC5, "eboot.bin", 5000),
                    self.put_dump("2026-10-13 14:09:48", 0x118DB, "shell.self", 7000)]
        self.new = self.put_dump("2026-10-20 12:30:00", 0x20000, "eboot.bin", 3000)

    def put(self, rel, body, when):
        p = self.device / rel
        p.write_bytes(body if isinstance(body, bytes) else body.encode())
        os.utime(p, (unix(when), unix(when)))
        return p

    def put_dump(self, crashed, pid, exe, size):
        t = unix(crashed)
        name = dump_name(t, pid, exe)
        p = self.device / "ux0/data" / name
        p.write_bytes(bytes(range(256)) * (size // 256) + b"\0" * (size % 256))
        os.utime(p, (t + 40, t + 40))
        return name

    def device_files(self):
        return {str(p.relative_to(self.device)): p.read_bytes() for p in self.device.rglob("*") if p.is_file()}

    def run_ftp(self, ftp, *args, env=None):
        return subprocess.run(["bash", str(self.package / "tools/install_ftp.sh"), *args, f"127.0.0.1:{ftp.port}"],
                              capture_output=True, text=True, timeout=180, cwd=self.root, stdin=subprocess.DEVNULL,
                              env=dict(os.environ, **(env or {})))

    def collect_folder(self):
        return max(f for f in (self.package / "build/device/collect").glob("vjo-collect-*") if f.is_dir())

    def test_collect_offers_and_archive_deletes_only_the_older_dumps(self):
        before = self.device_files()
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            port = ftp.port
            result = self.run_ftp(ftp, "--collect")
            commands = {line.split()[0].upper() for line in ftp.log}
            writes = list(ftp.stored)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # the collect and its offer wrote nothing: no terminal, so no question
        self.assertEqual(writes, [])
        self.assertFalse(commands & {"STOR", "APPE", "DELE", "MKD", "RMD", "RNFR", "RNTO", "SITE"}, commands)
        self.assertEqual(self.device_files(), before)
        out = result.stdout
        self.assertIn("crash dumps: 3 on the card, 1 NEW, from this boot, 2 from before it; this boot began between "
                      "2026-10-20 11:58:00 and 2026-10-20 12:01:04 by the Vita's clock", out)
        self.assertIn(f"crash dump from this boot, left on the Vita (this collect has it): {self.new}", out)
        self.assertIn("2 crash dump(s) from before this boot are on the Vita (12,000 B); this collect keeps a checked copy "
                      "of each", out)
        self.assertIn(f"  {self.old[0]} (5,000 B, an app or game (eboot.bin), 2026-10-11 13:57:17)", out)
        # the command keeps the port when it is not VitaShell's 1337
        self.assertIn(f"To archive them off the Vita: bash tools/install_ftp.sh --archive-dumps 127.0.0.1:{port}\n", out)
        self.assertNotIn("Archive them off the Vita?", out)
        self.assertLess(out.index("Send this one file:"), out.index("2 crash dump(s) from before"))
        folder = self.collect_folder()
        record = json.loads((folder / "COLLECT.json").read_text())
        self.assertEqual({e["name"]: e["date"] for e in record["listings"]["ux0:/data"]}[self.old[0]], "Oct 11 13:57")
        # --archive-dumps: the newest collect's older dumps, DELE alone, each confirmed gone
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            deletes = [line for line in ftp.log if line.upper().startswith("DELE")]
            commands = {line.split()[0].upper() for line in ftp.log}
            writes = list(ftp.stored)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(deletes, [f"DELE /ux0:/data/{n}" for n in self.old])
        self.assertEqual(writes, [f"DELE ux0/data/{n}" for n in self.old])
        self.assertFalse(commands & {"STOR", "APPE", "MKD", "RMD", "RNFR", "RNTO", "SITE"}, commands)
        after = self.device_files()
        self.assertEqual(after, {k: v for k, v in before.items() if k not in {f"ux0/data/{n}" for n in self.old}})
        self.assertIn(f"ux0/data/{self.new}", after)
        for n in self.old:
            self.assertIn(f"  deleted ux0:data/{n}", result.stdout)
            copy = folder / "ux0/data" / n
            self.assertEqual(hashlib.sha256(copy.read_bytes()).hexdigest(), record["files"][f"ux0:/data/{n}"]["sha256"])
        self.assertIn("archived: 2 deleted from the Vita;", result.stdout)
        log = (folder / "archive.log").read_text()
        self.assertEqual(log.count(" deleted ux0:data/psp2core-"), 2)
        self.assertIn(f"deleted ux0:data/{self.old[1]} (7000 B, sha256 {record['files'][f'ux0:/data/{self.old[1]}']['sha256']}, "
                      "SceShell, crashed 2026-10-13 14:09:48)", log)
        # again: nothing left to delete, nothing sent
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            deletes = [line for line in ftp.log if line.upper().startswith("DELE")]
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(deletes, [])
        self.assertIn(f"  already gone: {self.old[0]}", result.stdout)
        self.assertIn("archived: 0 deleted from the Vita, 2 already gone;", result.stdout)

    def test_a_dump_that_changed_or_a_refused_dele_stays(self):
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        grown = self.device / "ux0/data" / self.old[0]
        grown.write_bytes(grown.read_bytes() + b"more")
        with FtpMock(self.device, clock=lambda: self.VITA_NOW, fail={"DELE": lambda arg: True}) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            deletes = [line for line in ftp.log if line.upper().startswith("DELE")]
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(deletes, [f"DELE /ux0:/data/{self.old[1]}"])  # never one for the file that changed
        self.assertRegex(result.stdout, rf"  kept: {self.old[0]} \(on the Vita it is 5004 B, Oct \d+ \d\d:\d\d now, "
                                        r"not the collect's 5,000 B, Oct 11 13:57\)")
        self.assertIn(f"  kept: {self.old[1]} (DELE answered error_perm: 550 injected failure)", result.stdout)
        self.assertIn("archived: 0 deleted from the Vita, 2 left on it;", result.stdout)
        self.assertTrue((self.device / "ux0/data" / self.old[1]).is_file())

    def test_a_dele_answered_but_not_done_is_said(self):
        # the second listing, not DELE's answer, says whether a dump is gone
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        with FtpMock(self.device, clock=lambda: self.VITA_NOW, pretend={"DELE": lambda arg: True}) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        for n in self.old:
            self.assertIn(f"  kept: {n} (DELE was answered, but ux0:data still lists it)", result.stdout)
            self.assertTrue((self.device / "ux0/data" / n).is_file())
        self.assertIn("archived: 0 deleted from the Vita, 2 left on it;", result.stdout)
        self.assertEqual((self.collect_folder() / "archive.log").read_text().count("(still listed after DELE)"), 2)

    def test_a_dele_done_with_its_answer_lost_is_deleted(self):
        # audit N2: the DELE was carried out and the connection dropped before its answer; the second listing says gone
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        with FtpMock(self.device, clock=lambda: self.VITA_NOW, drop_after={"DELE": lambda arg: True}) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            deletes = [line for line in ftp.log if line.upper().startswith("DELE")]
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(deletes, [f"DELE /ux0:/data/{n}" for n in self.old])  # the second one on a new connection
        for n in self.old:
            self.assertIn(f"  deleted ux0:data/{n} (", result.stdout)
            self.assertFalse((self.device / "ux0/data" / n).exists())
        self.assertEqual(result.stdout.count("(its DELE's answer was lost)"), 2)
        self.assertIn("archived: 2 deleted from the Vita;", result.stdout)
        self.assertEqual((self.collect_folder() / "archive.log").read_text().count(" deleted ux0:data/"), 2)

    def test_a_dele_dropped_before_it_was_done_is_kept(self):
        # the connection dropped on the DELE, which was not carried out: the second listing still has the file
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        with FtpMock(self.device, clock=lambda: self.VITA_NOW, drop={"DELE": lambda arg: True}) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        for n in self.old:
            self.assertRegex(result.stdout, rf"  kept: {n} \(its DELE's answer was lost \(\w+Error: [^)]*\), and "
                                            r"ux0:data still lists it\)")
            self.assertTrue((self.device / "ux0/data" / n).is_file())
        self.assertIn("archived: 0 deleted from the Vita, 2 left on it;", result.stdout)

    def test_delta_audit_b2_end_to_end(self):
        # the delta audit's B2 over the FTP mock: this boot's Rubi-app dump, dated before the boot by a clock set back
        # during it, and kernel.txt ending with a raced game exit and a lone one: nothing is offered or deleted
        self.put("ux0/data/VitaJPOverlay/kernel.txt", KERNEL_HEAD + "game exit\ngame exit\n"
                 "foreground 0x20000 -> 0x0 (suspend) t=1800000 ms\ngame exit\n", "2026-10-20 12:30:20")
        self.put("ux0/data/VitaJPOverlay/status.txt", "bootstrap: shell API 22 entered\n", "2026-10-20 12:04:00")
        mine = self.put_dump("2026-10-20 11:10:00", 0x50000, "eboot.bin", 4000)
        before = self.device_files()
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--collect")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("the Vita's clock, NOT checked for this boot: kernel.txt ends with a game exit that may be a later "
                      "app's", result.stdout)
        self.assertNotIn("To archive them off the Vita", result.stdout)
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            deletes = [line for line in ftp.log if line.upper().startswith("DELE")]
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(deletes, [])
        self.assertIn("crash dumps: none from before this boot to archive", result.stdout)
        self.assertEqual(self.device_files(), before)
        self.assertTrue((self.device / "ux0/data" / mine).is_file())

    def test_a_lost_answer_then_no_listing_is_not_known(self):
        # audit-b N-c: each DELE is done and its answer lost, then ux0:data cannot be listed again: not known, never
        # "deleted" or "left on it"
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        holder = {}
        mock = FtpMock(self.device, clock=lambda: self.VITA_NOW, drop_after={"DELE": lambda arg: True},
                       fail={"LIST": lambda arg: bool(holder["ftp"].stored)})  # every LIST after the first DELE
        with mock as ftp:
            holder["ftp"] = ftp
            result = self.run_ftp(ftp, "--archive-dumps")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        for n in self.old:
            self.assertIn(f"  not known: {n} (its DELE's answer was lost, and ux0:data could not be listed again)",
                          result.stdout)
            self.assertFalse((self.device / "ux0/data" / n).exists())  # done, though not known
        self.assertIn("archived: 0 deleted from the Vita, 2 not known (a collect shows whether they are still there);",
                      result.stdout)
        log = (self.collect_folder() / "archive.log").read_text()
        self.assertEqual(log.count(" unknown psp2core-"), 2)
        self.assertNotIn(" deleted ux0:data/", log)

    def test_a_dump_whose_listed_time_changed_stays(self):
        # the same name and size, another LIST time: not the file this collect fetched
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        p = self.device / "ux0/data" / self.old[0]
        os.utime(p, (unix("2026-10-12 08:00:00"), unix("2026-10-12 08:00:00")))
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            deletes = [line for line in ftp.log if line.upper().startswith("DELE")]
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(deletes, [f"DELE /ux0:/data/{self.old[1]}"])
        self.assertIn(f"  kept: {self.old[0]} (on the Vita it is 5000 B, Oct 12 08:00 now, not the collect's 5,000 B, "
                      "Oct 11 13:57)", result.stdout)
        self.assertTrue(p.is_file())

    def test_another_vitas_collect_is_refused(self):
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        folder = self.collect_folder()
        record = json.loads((folder / "COLLECT.json").read_text())
        record["host"] = "192.0.2.9:1337"
        (folder / "COLLECT.json").write_text(json.dumps(record))
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            writes = list(ftp.stored)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(writes, [])
        self.assertIn(f"crash dumps: nothing archived: {folder.name} is a collect of 192.0.2.9:1337, not of 127.0.0.1; "
                      "collect this Vita first", result.stdout)

    def test_a_collect_without_a_host_is_refused(self):
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        folder = self.collect_folder()
        record = json.loads((folder / "COLLECT.json").read_text())
        del record["host"]
        (folder / "COLLECT.json").write_text(json.dumps(record))
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            writes = list(ftp.stored)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(writes, [])
        self.assertIn(f"crash dumps: nothing archived: {folder.name} is a collect of an unknown host, not of 127.0.0.1; "
                      "collect this Vita first", result.stdout)

    def test_ux0_data_cannot_be_listed_at_the_archive(self):
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        with FtpMock(self.device, clock=lambda: self.VITA_NOW, fail={"LIST": lambda arg: True}) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            writes = list(ftp.stored)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(writes, [])
        self.assertIn("cannot list ux0:data (550 injected failure): nothing deleted", result.stdout)

    def test_the_dumps_it_leaves_are_said_and_logged(self):
        unclear = self.put_dump("2026-10-20 12:00:50", 0x30000, "eboot.bin", 1000)
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            self.assertEqual(self.run_ftp(ftp, "--collect").returncode, 0)
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"crash dump left on the Vita (not placed before this boot): {unclear}", result.stdout)
        self.assertIn(f"crash dump from this boot, left on the Vita (this collect has it): {self.new}", result.stdout)
        log = (self.collect_folder() / "archive.log").read_text()
        self.assertIn(f" left {unclear} (not placed before this boot)\n", log)
        self.assertIn(f" left {self.new} (from this boot)\n", log)
        self.assertEqual(log.count(" deleted ux0:data/"), 2)
        self.assertTrue((self.device / "ux0/data" / unclear).is_file())

    def test_no_collect_to_archive_from(self):
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--archive-dumps")
            writes = list(ftp.stored)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f"No collect to archive from: run bash tools/install_ftp.sh --collect 127.0.0.1:{ftp.port} first",
                      result.stdout)
        self.assertEqual(writes, [])

    def test_no_older_dump_no_offer(self):
        for n in self.old:
            (self.device / "ux0/data" / n).unlink()
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            result = self.run_ftp(ftp, "--collect")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"crash dump from this boot, left on the Vita (this collect has it): {self.new}", result.stdout)
        self.assertNotIn("--archive-dumps", result.stdout)

    def in_pty(self, ftp, answer, env=None, stdout=None):
        """--collect in a terminal (both ends a pty), answering the archive question with `answer` (b"\x04": end of
        input). stdout: a file the script's standard output goes to instead, its standard input still the pty."""
        import pty
        import select
        argv = ["bash", str(self.package / "tools/install_ftp.sh"), "--collect", f"127.0.0.1:{ftp.port}"]
        if stdout is not None:
            argv = ["bash", "-c", 'exec "$@" > "$0"', str(stdout)] + argv
        environment, cwd = dict(os.environ, **(env or {})), str(self.root)
        pid, fd = pty.fork()
        if pid == 0:  # the child: the script on the pty, nothing else before exec
            os.chdir(cwd)
            os.execvpe("bash", argv, environment)
        out, deadline = b"", time.monotonic() + 120
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.5)
            if ready:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                out += chunk
                if b"[y/N] " in out and answer is not None:
                    os.write(fd, answer if isinstance(answer, bytes) else answer.encode() + b"\n")
                    answer = None
        os.close(fd)
        _, status = os.waitpid(pid, 0)
        return out.decode("utf-8", "replace"), os.waitstatus_to_exitcode(status)

    def test_the_question_in_a_terminal(self):
        before = self.device_files()
        for answer, env in (("", None), ("n", None), (None, {"VJO_NO_PROMPT": "1"})):
            with self.subTest(answer=answer, env=env), FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
                out, rc = self.in_pty(ftp, answer, env)
                writes = list(ftp.stored)
                self.assertEqual(rc, 0, out)
                self.assertEqual(writes, [])
                self.assertEqual(self.device_files(), before)
                if env:
                    self.assertNotIn("[y/N]", out)
                    self.assertIn("To archive them off the Vita: bash tools/install_ftp.sh --archive-dumps", out)
                else:
                    self.assertIn("Archive them off the Vita? This deletes them from ux0:data; this collect keeps them. "
                                  "[y/N]", out)
                    self.assertIn("Left on the Vita. To archive them later: bash tools/install_ftp.sh --archive-dumps", out)
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            out, rc = self.in_pty(ftp, "y")
            writes = list(ftp.stored)
        self.assertEqual(rc, 0, out)
        self.assertEqual(writes, [f"DELE ux0/data/{n}" for n in self.old])
        self.assertIn("archived: 2 deleted from the Vita;", out)
        self.assertTrue((self.device / "ux0/data" / self.new).is_file())

    def test_only_y_or_yes_in_a_terminal_archives(self):
        # audit mutants: end of input (Ctrl-D), and an answer that only starts with y, leave the Vita as it is
        before = self.device_files()
        for answer in (b"\x04", "yep", "no", "Y es"):
            with self.subTest(answer=answer), FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
                out, rc = self.in_pty(ftp, answer)
                writes = list(ftp.stored)
                self.assertEqual(rc, 0, out)
                self.assertIn("[y/N]", out)
                self.assertEqual(writes, [])
                self.assertEqual(self.device_files(), before)
                self.assertIn("Left on the Vita. To archive them later: bash tools/install_ftp.sh --archive-dumps", out)

    def test_no_question_when_stdout_is_not_a_terminal(self):
        # audit mutant: standard input a terminal, standard output a file (a log): no question, the command is printed
        before = self.device_files()
        log = self.root / "collect.log"
        with FtpMock(self.device, clock=lambda: self.VITA_NOW) as ftp:
            out, rc = self.in_pty(ftp, "y", stdout=log)
            writes = list(ftp.stored)
        self.assertEqual(rc, 0, out)
        self.assertEqual(writes, [])
        self.assertEqual(self.device_files(), before)
        text = log.read_text()
        self.assertNotIn("[y/N]", text + out)
        self.assertIn("To archive them off the Vita: bash tools/install_ftp.sh --archive-dumps", text)


if __name__ == "__main__":
    unittest.main()
