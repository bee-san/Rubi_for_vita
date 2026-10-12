"""Restart/crash tests of the offline Anki queue: every Vita-side action
runs in a fresh OS process (vjo-shell-anki-tests --restart-step).

The real Shell Anki thread, queue storage, screenshot capture and HTTP client
run with host SDK adapters. An AnkiConnect HTTP double stays alive across
client restarts, like Anki on the user's computer. These check process
crashes and disk recovery at each storage boundary, not electrical power
loss on physical Vita/SD2Vita hardware.
"""
import base64
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROBE = str(Path(sys.argv.pop(1)).resolve())


@contextmanager
def anki_server():
    state = {"notes": {}, "adds": 0, "actions": [], "drop_reply": False}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            action = request["action"]
            state["actions"].append(action)
            if action == "findNotes":
                query = request["params"]["query"]
                self.send_json([n["id"] for tag, n in state["notes"].items() if query == "tag:" + tag])
            elif action == "addNote":
                note = request["params"]["note"]
                tag = next(t for t in note["tags"] if t.startswith("vita_queue_"))
                state["adds"] += 1
                note_id = 1791436200000 + state["adds"]
                state["notes"][tag] = {"id": note_id, "note": request["params"]["note"]}
                if state["drop_reply"]:
                    state["drop_reply"] = False
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
                self.send_json(note_id)
            else:
                self.send_json(None, "unexpected action: " + action)

        def send_json(self, result, error=None):
            body = json.dumps({"result": result, "error": error}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        yield state, f"127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class RestartTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vjo-restart-")
        self.root = Path(self.temp.name)
        self.queue = self.root / "ux0:data" / "VitaJPOverlay" / "anki_queue"

    def tearDown(self):
        self.temp.cleanup()

    def step(self, action, crash="", host=None):
        args = [PROBE, "--restart-step", str(self.root), action, crash]
        if host is not None:
            args.append(host)
        result = subprocess.run(args, text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 86 if crash else 0, result.stdout + result.stderr)
        return None if crash else json.loads(result.stdout)

    def snapshot(self):
        return {p.name: p.read_bytes() for p in self.queue.iterdir() if p.is_file()}

    def test_confirmed_save_survives_fresh_processes_with_screenshot(self):
        self.assertEqual(self.step("save"), {"status": "Saved for Anki", "count": 1})
        first = self.step("inspect")
        self.assertGreater(first["picture_len"], 1000)
        for _ in range(3):
            restored = self.step("inspect")
            self.assertEqual(restored, first)
            self.assertEqual(restored["count"], 1)
            self.assertEqual(restored["note"]["spelling"], "猫")
            self.assertEqual(restored["note"]["reading"], "ねこ")
            self.assertEqual(restored["note"]["sentence"], "猫が好き猫")
            self.assertEqual(restored["note"]["meanings"], ["cat", 'shamisen "neko"'])
            self.assertEqual(restored["note"]["misc"], "CLANNAD (2026-10-12 09:30)")
        # The same card again is recognized, in another process.
        self.assertEqual(self.step("save"), {"status": "Already queued", "count": 1})
        with anki_server() as (state, host):
            self.step("inspect")
            self.assertEqual(state["actions"], [])  # reopening does not send
            self.assertEqual(self.step("sync", host=host), {"count": 0, "status": "Sent 1 to Anki"})
            sent = next(iter(state["notes"].values()))["note"]
            picture = base64.b64decode(sent["picture"][0]["data"])
            self.assertEqual(hashlib.sha256(picture).hexdigest()[:32], first["picture_hash"])
            self.assertEqual(len(picture), first["picture_len"])
            self.assertEqual(sent["fields"]["Expression"], "猫")
        self.assertEqual(self.step("inspect"), {"count": 0})

    def test_interrupted_save_never_damages_previously_saved_cards(self):
        checkpoints = ("jpeg_partial", "jpeg_flushed", "jpeg_renamed", "json_partial",
                       "json_flushed", "json_renamed", "json_committed")
        for point in checkpoints:
            with self.subTest(checkpoint=point):
                # A separate storage directory for every crash boundary.
                self.root = Path(self.temp.name) / point
                self.root.mkdir()
                self.queue = self.root / "ux0:data" / "VitaJPOverlay" / "anki_queue"
                self.step("save")
                original = self.snapshot()
                self.step("save-other", crash=point)
                for name, data in original.items():
                    self.assertEqual((self.queue / name).read_bytes(), data)
                restored = self.step("inspect")
                # A final JSON is complete; unfinished .tmp records are ignored.
                expected = 2 if point in ("json_renamed", "json_committed") else 1
                self.assertEqual(restored["count"], expected)
                with anki_server() as (state, host):
                    self.assertEqual(self.step("sync", host=host)["count"], 0)
                    self.assertEqual(state["adds"], expected)
                self.assertEqual(self.step("inspect"), {"count": 0})

    def test_restart_after_anki_accepts_before_local_cleanup(self):
        self.step("save")
        with anki_server() as (state, host):
            self.step("sync", crash="before_json_remove", host=host)
            self.assertEqual(state["adds"], 1)
            self.assertEqual(self.step("inspect")["count"], 1)
            self.assertEqual(self.step("sync", host=host)["count"], 0)
            self.assertEqual(state["adds"], 1)
            self.assertEqual(state["actions"], ["findNotes", "addNote", "findNotes"])

    def test_restart_after_lost_anki_response(self):
        self.step("save")
        with anki_server() as (state, host):
            state["drop_reply"] = True
            self.assertEqual(self.step("sync", host=host)["count"], 1)
            self.assertEqual(state["adds"], 1)
            self.assertEqual(self.step("inspect")["count"], 1)
            self.assertEqual(self.step("sync", host=host), {"count": 0, "status": "Sent 1 to Anki"})
            self.assertEqual(state["adds"], 1)

    def test_restart_during_cleanup_leaves_no_broken_pending_record(self):
        for point in ("after_json_remove", "before_jpeg_remove"):
            with self.subTest(checkpoint=point):
                self.root = Path(self.temp.name) / point
                self.root.mkdir()
                self.queue = self.root / "ux0:data" / "VitaJPOverlay" / "anki_queue"
                self.step("save")
                with anki_server() as (state, host):
                    self.step("sync", crash=point, host=host)
                    self.assertEqual(self.step("inspect"), {"count": 0})
                    self.assertEqual(self.step("sync", host=host)["count"], 0)
                    self.assertEqual(state["adds"], 1)

    def test_reappearing_record_after_unflushed_delete_is_not_resent(self):
        self.step("save")
        original = self.snapshot()
        with anki_server() as (state, host):
            self.step("sync", crash="after_json_remove", host=host)
            # Model the other possible reboot outcome: the directory deletion
            # had not reached disk. The saved JSON and JPEG reappear together.
            for name, data in original.items():
                (self.queue / name).write_bytes(data)
            self.assertEqual(self.step("inspect")["count"], 1)
            self.assertEqual(self.step("sync", host=host)["count"], 0)
            self.assertEqual(state["adds"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
