"""A small FTP server for the host tests, shaped like VitaShell's (ftpvita): anonymous login,
the root lists the devices (`ux0:`, `ur0:`), paths are `/ux0:/data/...` or relative to the
current directory, passive mode only (PASV and EPSV), LIST (no NLST), RETR, STOR, SIZE, DELE,
MKD, RMD, CWD/CDUP/PWD. A device path maps onto ROOT/<device>/... (`ux0:` -> ROOT/ux0), so
the real curl and Python's ftplib (what tools/install_ftp.sh and tools/collect_vita.py use)
run against files the test can read and write.

    with FtpMock(root) as ftp:          # ftp.port, ftp.log (the commands received)
        ...

Not a general FTP server: one client at a time per connection thread, binary transfers,
127.0.0.1 only.
"""
import socket
import socketserver
import threading
import time
from pathlib import Path


class _Handler(socketserver.StreamRequestHandler):
    def setup(self):
        super().setup()
        self.cwd = "/"
        self.pasv = None

    def finish(self):
        if self.pasv:  # an EPSV/PASV the client never used (curl, for a file that is not there)
            self.pasv.close()
            self.pasv = None
        super().finish()

    # ---- paths ----
    def resolve(self, arg):
        """FTP path -> (virtual path, local Path or None for the device root)."""
        path = arg if arg.startswith("/") else (self.cwd.rstrip("/") + "/" + arg)
        parts = []
        for p in path.split("/"):
            if p in ("", "."):
                continue
            if p == "..":
                if parts:
                    parts.pop()
                continue
            parts.append(p)
        virtual = "/" + "/".join(parts)
        if not parts:
            return virtual, None
        dev = parts[0]
        if not dev.endswith(":") or dev[:-1] not in self.server.devices:
            return virtual, False
        return virtual, self.server.root.joinpath(dev[:-1], *parts[1:])

    def reply(self, text):
        self.wfile.write((text + "\r\n").encode())

    def data_conn(self):
        if not self.pasv:
            return None
        self.pasv.settimeout(10)
        try:
            conn, _ = self.pasv.accept()
        finally:
            self.pasv.close()
            self.pasv = None
        return conn

    def open_pasv(self):
        if self.pasv:
            self.pasv.close()
        self.pasv = socket.socket()
        self.pasv.bind(("127.0.0.1", 0))
        self.pasv.listen(1)
        return self.pasv.getsockname()[1]

    @staticmethod
    def list_line(p):
        st = p.stat()
        mode = "drwxrwxrwx" if p.is_dir() else "-rw-rw-rw-"
        stamp = time.strftime("%b %d %H:%M", time.gmtime(st.st_mtime))
        return f"{mode}   1 root  root  {st.st_size:>10} {stamp} {p.name}"

    def handle(self):
        server = self.server
        self.reply("220 FTPVita Server ready.")
        while True:
            raw = self.rfile.readline()
            if not raw:
                return
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            cmd, _, arg = line.partition(" ")
            cmd = cmd.upper()
            server.log.append(line)
            if server.fail and cmd in server.fail and server.fail[cmd](arg):
                self.reply("550 injected failure")
                continue
            if cmd in ("USER", "PASS"):
                self.reply("331 Password required" if cmd == "USER" else "230 User logged in")
            elif cmd in ("SYST",):
                self.reply("215 UNIX Type: L8")
            elif cmd in ("TYPE", "MODE", "STRU", "NOOP", "OPTS"):
                self.reply("200 OK")
            elif cmd == "PWD":
                self.reply(f'257 "{self.cwd}" is the current directory')
            elif cmd in ("CWD", "CDUP"):
                virtual, local = self.resolve(".." if cmd == "CDUP" else arg)
                if local is None or (local and local.is_dir()):
                    self.cwd = virtual
                    self.reply("250 Requested file action okay, completed.")
                else:
                    self.reply("550 Invalid directory.")
            elif cmd == "PASV":
                port = self.open_pasv()
                self.reply(f"227 Entering Passive Mode (127,0,0,1,{port >> 8},{port & 255})")
            elif cmd == "EPSV":
                port = self.open_pasv()
                self.reply(f"229 Entering Extended Passive Mode (|||{port}|)")
            elif cmd == "PORT" or cmd == "EPRT" or cmd == "NLST" or cmd == "FEAT" or cmd == "MDTM":
                self.reply("502 Command not implemented.")
            elif cmd == "LIST":
                target = arg if arg and not arg.startswith("-") else ""
                virtual, local = self.resolve(target or ".")
                if local is False or (local and not local.is_dir()):
                    self.reply("550 Invalid directory.")
                    continue
                conn = self.data_conn()
                if not conn:
                    self.reply("425 No data connection")
                    continue
                self.reply("150 Opening ASCII mode data transfer for LIST.")
                if local is None:
                    rows = [f"drwxrwxrwx   1 root  root           0 Jan 01 00:00 {d}:" for d in server.devices
                            if (server.root / d).is_dir()]
                else:
                    rows = [self.list_line(p) for p in sorted(local.iterdir())]
                conn.sendall("".join(r + "\r\n" for r in rows).encode())
                conn.close()
                self.reply("226 Transfer complete.")
            elif cmd == "SIZE":
                _, local = self.resolve(arg)
                if local and local.is_file():
                    self.reply(f"213 {local.stat().st_size}")
                else:
                    self.reply("550 Could not get file size.")
            elif cmd == "RETR":
                _, local = self.resolve(arg)
                if not (local and local.is_file()):
                    self.reply("550 File not found.")
                    continue
                conn = self.data_conn()
                if not conn:
                    self.reply("425 No data connection")
                    continue
                self.reply("150 Opening Image mode data transfer.")
                conn.sendall(local.read_bytes())
                conn.close()
                server.retrieved.append(str(local.relative_to(server.root)))
                self.reply("226 Transfer complete.")
            elif cmd in ("STOR", "APPE"):
                _, local = self.resolve(arg)
                if not local or not local.parent.is_dir():
                    self.reply("550 Invalid path.")
                    continue
                conn = self.data_conn()
                if not conn:
                    self.reply("425 No data connection")
                    continue
                self.reply("150 Opening Image mode data transfer.")
                chunks = []
                while True:
                    b = conn.recv(65536)
                    if not b:
                        break
                    chunks.append(b)
                conn.close()
                with local.open("ab" if cmd == "APPE" else "wb") as out:
                    out.write(b"".join(chunks))
                server.stored.append(str(local.relative_to(server.root)))
                self.reply("226 Transfer complete.")
            elif cmd == "DELE":
                _, local = self.resolve(arg)
                if local and local.is_file():
                    local.unlink()
                    server.stored.append("DELE " + str(local.relative_to(server.root)))
                    self.reply("226 File deleted.")  # VitaShell's code, which ftplib calls an error_reply
                else:
                    self.reply("550 Could not delete the file.")
            elif cmd == "MKD":
                _, local = self.resolve(arg)
                if local and not local.exists():
                    local.mkdir(parents=False)
                    server.stored.append("MKD " + str(local.relative_to(server.root)))
                    self.reply(f'257 "{arg}" created')
                else:
                    self.reply("550 Could not create the directory.")
            elif cmd == "RMD":
                _, local = self.resolve(arg)
                try:
                    local.rmdir()
                    self.reply("226 Directory removed.")
                except (OSError, AttributeError):
                    self.reply("550 Could not remove the directory.")
            elif cmd == "QUIT":
                self.reply("221 Goodbye.")
                return
            else:
                self.reply("502 Command not implemented.")


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class FtpMock:
    """A VitaShell-like FTP server over ROOT/ux0, ROOT/ur0 on 127.0.0.1 (port 0: any free port).
    fail: {COMMAND: predicate(arg)} makes matching commands answer 550 (an unreadable file)."""

    def __init__(self, root, devices=("ux0", "ur0"), fail=None):
        self.server = _Server(("127.0.0.1", 0), _Handler)
        self.server.root = Path(root)
        self.server.devices = devices
        self.server.log = []
        self.server.retrieved = []
        self.server.stored = []
        self.server.fail = fail or {}
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)

    @property
    def log(self):
        return self.server.log

    @property
    def stored(self):
        """Every STOR/APPE/DELE/MKD: what a read-only client must never do."""
        return self.server.stored

    @property
    def retrieved(self):
        return self.server.retrieved

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
