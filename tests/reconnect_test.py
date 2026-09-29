#!/usr/bin/env python3
"""Exercise a stable device symlink across absent startup and PTY replacement."""
import fcntl
import os
from pathlib import Path
import pty
import socket
import sqlite3
import struct
import subprocess
import tempfile
import termios
import threading
import time

from serial_latency_test import RadioSimulator, ipc_send, ipc_recv, wire_frame
from tui_smoke_test import read_available, render_terminal


class Radio(RadioSimulator):
    def __init__(self, master):
        super().__init__(master)
        self.hold_send = False
        self.held = threading.Event()

    def handle_command(self, payload):
        if payload[0] == 3 and self.hold_send:
            self.commands.append(3)
            self.held.set()
        else:
            super().handle_command(payload)


def main():
    binary = str(Path(os.environ["QTC_BIN"]).resolve())
    version = subprocess.check_output([binary, "--version"], text=True).split()[1]
    with tempfile.TemporaryDirectory(prefix="qtc-reconnect-", dir="/tmp") as tmp:
        root = Path(tmp)
        env = os.environ.copy()
        for key, name in (("XDG_RUNTIME_DIR", "run"), ("XDG_DATA_HOME", "data"),
                          ("XDG_CONFIG_HOME", "config")):
            env[key] = str(root / name)
            (root / name).mkdir()
        device = root / "stable-by-id"
        command = [binary, "--profile", "reconnect"]
        sockpath = root / "run/qtc/reconnect/qtc.sock"
        dbpath = root / "data/qtc-terminal/reconnect/qtc.db"
        radio = None
        master = slave = tui_master = tui_slave = -1
        client = tui = None
        with (root / "core.log").open("w+") as log:
            core = subprocess.Popen(command + ["core", "--foreground", "--debug", "--device", str(device)],
                                    env=env, stdout=subprocess.DEVNULL, stderr=log)
            try:
                def status():
                    result = subprocess.run(command + ["status"], env=env, capture_output=True,
                                            text=True, timeout=3)
                    assert core.poll() is None, (core.returncode, log.read())
                    if result.returncode:
                        return {}
                    fields = dict(line.split(": ", 1) for line in result.stdout.splitlines())
                    assert fields["PID"] == str(core.pid)
                    assert fields["Device"] == str(device)
                    return fields

                def wait_for(check, timeout=8):
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        if tui is not None:
                            assert tui.poll() is None
                            read_available(tui_master, 0.02)
                        if check():
                            return
                        time.sleep(0.05)
                    raise AssertionError(f"condition timed out; status={status()}")

                wait_for(lambda: status().get("Session") == "reconnecting")
                assert status()["Radio"] == "disconnected"
                # A debug line per open makes retry pacing observable without strace.
                time.sleep(3.3)
                opens = (root / "core.log").read_text().count("Opening radio path:")
                assert opens == 2, opens
                client = socket.socket(socket.AF_UNIX)
                client.connect(str(sockpath))
                ipc_send(client, 1, struct.pack("<I32s16s", 2, b"reconnect-test", version.encode()))
                while ipc_recv(client)[0] != 7:
                    pass

                def ping():
                    ipc_send(client, 33)
                    seen_info = False
                    while True:
                        kind, payload = ipc_recv(client)
                        seen_info |= kind == 10
                        if kind == 8 and seen_info:
                            return payload[0]

                def reject_offline():
                    ipc_send(client, 21, struct.pack("<i768s", 0, b"must not queue offline"))
                    while True:
                        kind, payload = ipc_recv(client)
                        if kind == 255:
                            assert b"Radio unavailable" in payload
                            return

                assert ping() == 0
                reject_offline()
                tui_master, tui_slave = pty.openpty()
                fcntl.ioctl(tui_slave, termios.TIOCSWINSZ, struct.pack("HHHH", 28, 100, 0, 0))
                tui = subprocess.Popen(command, env=env, stdin=tui_slave, stdout=tui_slave, stderr=tui_slave)
                os.close(tui_slave); tui_slave = -1
                assert b"QTC TERMINAL" in read_available(tui_master, 0.8)

                def sql(query):
                    with sqlite3.connect(dbpath) as db:
                        return db.execute(query).fetchall()

                offline_fds = len(list(Path(f"/proc/{core.pid}/fd").iterdir())) if Path("/proc").exists() else None
                for cycle in range(3):
                    master, slave = pty.openpty()
                    device.symlink_to(os.ttyname(slave))
                    radio = Radio(master); radio.start()
                    wait_for(lambda: status().get("Session") == "ready")
                    assert radio.commands[:2] == [1, 22], radio.commands
                    assert ping() == 1
                    assert status()["Node"] == "Latency Radio"
                    assert status()["Firmware"] == "test-1.0"
                    wait_for(lambda: 31 in radio.commands)
                    wait_for(lambda: bool(sql("SELECT * FROM channels WHERE configured=1")))
                    if cycle:
                        assert 3 not in radio.commands, "old in-flight message replayed"
                    radio.queue_incoming(f"received after reconnect {cycle}")
                    wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='received after reconnect {cycle}'")))
                    # Real outbound success following each clean session initialization.
                    ipc_send(client, 21, struct.pack("<i768s", 0, f"sent after reconnect {cycle}".encode()))
                    wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='sent after reconnect {cycle}' AND status=2")))
                    radio.hold_send = True
                    ipc_send(client, 21, struct.pack("<i768s", 0, f"ambiguous {cycle}".encode()))
                    wait_for(radio.held.is_set)
                    ipc_send(client, 21, struct.pack("<i768s", 0, f"queued {cycle}".encode()))
                    wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='queued {cycle}' AND status=0")))
                    # Leave an incomplete radio frame buffered before losing this tty.
                    os.write(master, wire_frame(b"\x05" + b"x" * 100)[:11])
                    time.sleep(0.1)
                    device.unlink()
                    radio.close(); radio.join(timeout=2)
                    assert not radio.is_alive()
                    os.close(master); os.close(slave); master = slave = -1
                    radio = None
                    wait_for(lambda: status().get("Session") == "reconnecting")
                    assert status()["Radio"] == "disconnected"
                    assert "Node" not in status() and "Firmware" not in status()
                    assert ping() == 0
                    reject_offline()
                    wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='ambiguous {cycle}' AND status=4 AND ack_deadline=0")))
                    assert sql(f"SELECT 1 FROM messages WHERE text='queued {cycle}' AND status=5 AND ack_deadline=0")
                    assert not sql("SELECT 1 FROM messages WHERE text='must not queue offline'")
                    if offline_fds is not None:
                        assert len(list(Path(f"/proc/{core.pid}/fd").iterdir())) == offline_fds
                    os.write(tui_master, b"\x1b[18~")
                    frame = read_available(tui_master, 0.3)
                    assert "NETWORK NODES" in "\n".join(render_terminal(frame))
                    os.write(tui_master, b"\x1b[18~")
                    read_available(tui_master, 0.2)
                print("serial reconnect tests passed (startup absent, three sessions, stable PID/IPC/TUI)")
            finally:
                if radio is not None:
                    radio.close(); radio.join(timeout=2)
                if client is not None:
                    client.close()
                if tui is not None:
                    tui.terminate(); tui.wait(timeout=5)
                core.terminate(); core.wait(timeout=5)
                for fd in (master, slave, tui_master, tui_slave):
                    if fd >= 0:
                        os.close(fd)


if __name__ == "__main__":
    main()
