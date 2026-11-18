#!/usr/bin/env python3
"""
Web interface for myshell.

Runs the real ./myshell binary inside a pseudo-terminal (pty) and streams it
to the browser over a WebSocket. Because the shell gets a genuine terminal,
job control (tcsetpgrp, Ctrl+C, Ctrl+Z, fg, bg) works exactly as in a normal
terminal window.

It also reads /proc every half second and sends the browser a live table of
the shell's child processes (PID, PGID, state, terminal owner), so you can
watch process groups and job control happen.

Usage:
    pip install aiohttp
    python3 web/server.py            # then open http://localhost:8080
    python3 web/server.py --port 9000
"""
import argparse
import asyncio
import fcntl
import json
import os
import pty
import signal
import struct
import subprocess
import termios
from pathlib import Path

from aiohttp import WSMsgType, web

ROOT = Path(__file__).resolve().parent.parent      # repo root
STATIC = Path(__file__).resolve().parent / "static"
SOURCE = ROOT / "shell.cpp"
BINARY = ROOT / "myshell"


# ── Build ──────────────────────────────────────────────────────────────────
def build_shell():
    """Compile shell.cpp if the binary is missing or older than the source."""
    if BINARY.exists() and BINARY.stat().st_mtime >= SOURCE.stat().st_mtime:
        return
    print("Compiling shell.cpp ...")
    subprocess.run(["g++", "-std=c++17", "-o", str(BINARY), str(SOURCE)], check=True)


# ── /proc reader: live view of the shell's processes ───────────────────────
def read_stat(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            data = f.read()
    except OSError:
        return None
    # Format: pid (comm) state ppid pgrp session tty_nr tpgid ...
    # comm may contain spaces/parentheses, so split around the last ')'.
    l, r = data.index("("), data.rindex(")")
    fields = data[r + 2:].split()
    return {
        "pid": pid,
        "comm": data[l + 1:r],
        "state": fields[0],
        "ppid": int(fields[1]),
        "pgid": int(fields[2]),
        "tpgid": int(fields[5]),   # foreground process group of the terminal
    }


def read_cmdline(pid):
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            raw = f.read()
    except OSError:
        return ""
    return raw.replace(b"\0", b" ").decode(errors="replace").strip()


def process_tree(root_pid):
    """Return the shell and all its descendants, depth-first."""
    if not os.path.isdir("/proc"):
        return []
    procs = {}
    for name in os.listdir("/proc"):
        if name.isdigit():
            st = read_stat(int(name))
            if st:
                procs[st["pid"]] = st
    if root_pid not in procs:
        return []
    children = {}
    for p in procs.values():
        children.setdefault(p["ppid"], []).append(p["pid"])
    out, stack = [], [(root_pid, 0)]
    while stack:
        pid, depth = stack.pop()
        p = procs[pid]
        p["depth"] = depth
        p["cmd"] = read_cmdline(pid) or p["comm"]
        out.append(p)
        for c in sorted(children.get(pid, []), reverse=True):
            stack.append((c, depth + 1))
    return out


# ── One browser tab = one shell session ────────────────────────────────────
class ShellSession:
    def __init__(self, ws):
        self.ws = ws
        self.pid = None
        self.fd = None
        self.out_queue = asyncio.Queue()
        self.exited = False

    def spawn(self):
        pid, fd = pty.fork()   # child gets a new session with the pty as its controlling terminal
        if pid == 0:
            os.chdir(ROOT)
            os.environ["TERM"] = "xterm-256color"
            os.environ.setdefault("HOME", str(Path.home()))
            os.execv(str(BINARY), ["./myshell"])
        self.pid, self.fd = pid, fd
        os.set_blocking(fd, False)
        asyncio.get_running_loop().add_reader(fd, self._on_readable)

    def _on_readable(self):
        try:
            data = os.read(self.fd, 65536)
        except OSError:
            data = b""
        if data:
            self.out_queue.put_nowait(data)
        else:
            asyncio.get_running_loop().remove_reader(self.fd)

    def resize(self, rows, cols):
        if self.fd is not None:
            fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def write(self, text):
        if self.fd is not None and not self.exited:
            try:
                os.write(self.fd, text.encode())
            except OSError:
                pass

    def check_exit(self):
        if self.exited:
            return True
        try:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
        except ChildProcessError:
            pid = self.pid
        if pid == self.pid:
            self.exited = True
        return self.exited

    def cleanup(self):
        # Kill anything the shell left running (background jobs, stopped jobs).
        try:
            for p in process_tree(self.pid):
                try:
                    os.kill(p["pid"], signal.SIGKILL)
                except OSError:
                    pass
        except Exception:
            pass
        try:
            asyncio.get_running_loop().remove_reader(self.fd)
        except Exception:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass
        if not self.exited:
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass


async def sender(session):
    """Forward terminal output to the browser in order."""
    while True:
        data = await session.out_queue.get()
        await session.ws.send_bytes(data)


async def monitor(session):
    """Send the live process table and detect when the shell exits."""
    last = None
    while True:
        await asyncio.sleep(0.5)
        if session.check_exit():
            await asyncio.sleep(0.2)   # let the last output ("Goodbye!") flush
            await session.ws.send_str(json.dumps({"type": "exit"}))
            await session.ws.close()
            return
        procs = process_tree(session.pid)
        msg = json.dumps({"type": "procs", "shell": session.pid, "procs": procs})
        if msg != last:
            await session.ws.send_str(msg)
            last = msg


async def ws_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    session = ShellSession(ws)
    session.spawn()
    await ws.send_str(json.dumps({"type": "hello", "cwd": str(ROOT), "pid": session.pid}))
    tasks = [asyncio.create_task(sender(session)), asyncio.create_task(monitor(session))]

    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            data = json.loads(msg.data)
            if data.get("type") == "input":
                session.write(data.get("data", ""))
            elif data.get("type") == "resize":
                session.resize(int(data["rows"]), int(data["cols"]))
    finally:
        for t in tasks:
            t.cancel()
        session.cleanup()
    return ws


async def index(_request):
    return web.FileResponse(STATIC / "index.html")


def main():
    parser = argparse.ArgumentParser(description="Web interface for myshell")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="127.0.0.1",
                        help="default 127.0.0.1 (local only). Anyone who can reach this port gets a shell.")
    args = parser.parse_args()

    if not Path("/proc").is_dir():
        print("Note: /proc not found (not Linux) - the live process panel will stay empty.")
    build_shell()

    app = web.Application()
    app.router.add_get("/", index)
    app.router.add_get("/ws", ws_handler)
    app.router.add_static("/static/", STATIC)
    print(f"myshell web interface running at http://localhost:{args.port}")
    web.run_app(app, host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
