#!/usr/bin/env python3
"""A missed inbox response must not reopen a healthy READY radio session."""
import os
from pathlib import Path
import pty
import socket
import sqlite3
import struct
import subprocess
import tempfile
import threading
import time

from serial_latency_test import RadioSimulator, ipc_send, ipc_recv, wait_for_socket


class Radio(RadioSimulator):
    def __init__(self, master):
        super().__init__(master)
        self.drop_next = threading.Event()
        self.dropped = threading.Event()

    def handle_command(self, payload):
        if payload[0] == 10 and self.drop_next.is_set():
            self.drop_next.clear()
            self.commands.append(10)
            self.dropped.set()
            return
        super().handle_command(payload)


def main():
    binary = str(Path(os.environ["QTC_BIN"]).resolve())
    version = subprocess.check_output([binary, "--version"], text=True).split()[1]
    with tempfile.TemporaryDirectory(prefix="qtc-command-timeout-", dir="/tmp") as tmp:
        root = Path(tmp)
        env = os.environ.copy()
        for key, folder in (("XDG_RUNTIME_DIR", "run"), ("XDG_DATA_HOME", "data"),
                            ("XDG_CONFIG_HOME", "config")):
            env[key] = str(root / folder)
            (root / folder).mkdir()
        master, slave = pty.openpty()
        radio = Radio(master)
        radio.start()
        device = root / "stable-by-id"
        device.symlink_to(os.ttyname(slave))
        command = [binary, "--profile", "timeout"]
        with (root / "core.log").open("w") as log:
            core = subprocess.Popen(command + ["core", "--foreground", "--debug", "--device", str(device)],
                                    env=env, stdout=subprocess.DEVNULL, stderr=log)
            client = socket.socket(socket.AF_UNIX)
            try:
                path = root / "run/qtc/timeout/qtc.sock"
                wait_for_socket(path)
                client.connect(str(path))
                ipc_send(client, 1, struct.pack("<I32s16s", 2, b"timeout-test", version.encode()))
                while ipc_recv(client)[0] != 7:
                    pass

                def wait_for(check):
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        assert core.poll() is None
                        if check():
                            return
                        time.sleep(0.05)
                    raise AssertionError(("condition timed out", (root / "core.log").read_text()))

                def sql(query):
                    with sqlite3.connect(root / "data/qtc-terminal/timeout/qtc.db") as db:
                        return db.execute(query).fetchall()

                wait_for(lambda: bool(sql("SELECT 1 FROM channels WHERE configured=1")))
                for cycle in range(3):
                    radio.dropped.clear()
                    radio.drop_next.set()
                    wait_for(radio.dropped.is_set)
                    deadline = time.monotonic() + 5
                    while True:
                        kind, payload = ipc_recv(client, max(0.01, deadline - time.monotonic()))
                        if kind == 8 and b"timed out" in payload[-160:]:
                            assert payload[0] == 1, "isolated timeout disconnected radio"
                            assert b"session remains ready" in payload[-160:]
                            break
                        assert time.monotonic() < deadline
                    # Successful commands and RX between losses reset the streak.
                    radio.queue_incoming(f"incoming after timeout {cycle}")
                    wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='incoming after timeout {cycle}'")))
                    ipc_send(client, 21, struct.pack("<i768s", 0, f"outgoing after timeout {cycle}".encode()))
                    wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='outgoing after timeout {cycle}' AND status=2")))
                    result = subprocess.run(command + ["status"], env=env, capture_output=True, text=True, timeout=3)
                    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
                    assert f"PID: {core.pid}\n" in result.stdout, result.stdout
                    assert "Session: ready\n" in result.stdout, result.stdout
                    assert "Radio: connected\n" in result.stdout, result.stdout
                    assert radio.commands.count(1) == 1 and radio.commands.count(22) == 1, radio.commands
                    assert (root / "core.log").read_text().count("Opening radio path:") == 1
                print("command timeout PTY test passed (three isolated losses, no reopen, RX/TX recovered)")
            finally:
                client.close()
                core.terminate(); core.wait(timeout=5)
                radio.close(); radio.join(timeout=2)
                os.close(master); os.close(slave)


if __name__ == "__main__":
    main()
