#!/usr/bin/env python3
"""Delayed inbox replies retain command ownership; exhaustion recovers the session."""
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
        self.mode = None
        self.seen = threading.Event()
        self.replied = threading.Event()
        self.held = False
        self.interleaved = []

    def handle_command(self, payload):
        if self.held:
            self.interleaved.append(payload[0])
        if payload[0] == 10 and self.mode:
            mode, self.mode = self.mode, None
            self.commands.append(10)
            self.held = True
            self.seen.set()
            if mode == "exhaust":
                return
            if mode == "message":
                self.queue_incoming("delayed inbox message", announce=False)
            with self.incoming_lock:
                response = self.incoming_payloads.popleft() if self.incoming_payloads else bytes([10])
            def reply():
                self.held = False
                self.send(response)
                self.replied.set()
            threading.Timer(0.65, reply).start()
            return
        if payload[0] == 1:
            self.held = False
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
                for cycle, mode in enumerate(("empty", "message", "exhaust")):
                    radio.seen.clear(); radio.replied.clear(); radio.interleaved.clear()
                    radio.mode = mode
                    wait_for(radio.seen.is_set)
                    if mode != "exhaust":
                        ipc_send(client, 21, struct.pack("<i768s", 0, f"queued during delay {cycle}".encode()))
                        wait_for(radio.replied.is_set)
                        assert not radio.interleaved, radio.interleaved
                        wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='queued during delay {cycle}' AND status=2")))
                        if mode == "message":
                            wait_for(lambda: bool(sql("SELECT 1 FROM messages WHERE text='delayed inbox message'")))
                        assert radio.commands.count(1) == 1, radio.commands
                    else:
                        deadline = time.monotonic() + 5
                        while True:
                            kind, payload = ipc_recv(client, max(0.01, deadline - time.monotonic()))
                            if kind == 8 and b"timed out; reconnecting" in payload[-160:]:
                                assert payload[0] == 0, payload
                                break
                            assert time.monotonic() < deadline
                        # A stale empty reply arrives after exhaustion, before reopen.
                        # Existing serial-open flushing/handshake must isolate it.
                        radio.send(bytes([10]))
                        wait_for(lambda: radio.commands.count(22) == 2)
                        # No second inbox request or unrelated command before reinitialization.
                        assert radio.interleaved == [1], radio.interleaved
                    radio.queue_incoming(f"incoming after operation {cycle}")
                    wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='incoming after operation {cycle}'")))
                    ipc_send(client, 21, struct.pack("<i768s", 0, f"outgoing after operation {cycle}".encode()))
                    wait_for(lambda: bool(sql(f"SELECT 1 FROM messages WHERE text='outgoing after operation {cycle}' AND status=2")))
                    result = subprocess.run(command + ["status"], env=env, capture_output=True, text=True, timeout=3)
                    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
                    assert f"PID: {core.pid}\n" in result.stdout, result.stdout
                    assert "Session: ready\n" in result.stdout, result.stdout
                    assert "Radio: connected\n" in result.stdout, result.stdout
                    expected = 2 if mode == "exhaust" else 1
                    assert radio.commands.count(1) == expected and radio.commands.count(22) == expected, radio.commands
                    assert (root / "core.log").read_text().count("Opening radio path:") == expected
                print("command timeout PTY test passed (delayed replies serialized, exhaustion recovered, RX/TX usable)")
            finally:
                client.close()
                core.terminate(); core.wait(timeout=5)
                radio.close(); radio.join(timeout=2)
                os.close(master); os.close(slave)


if __name__ == "__main__":
    main()
