#!/usr/bin/env python3
"""Status CLI and optional PING extension, using isolated real cores and replay."""
import os
import platform
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


def receive(sock):
    def exact(size):
        data = b""
        while len(data) < size:
            chunk = sock.recv(size - len(data))
            if not chunk:
                raise RuntimeError("truncated IPC frame")
            data += chunk
        return data
    header = exact(5)
    length, kind = struct.unpack("<IB", header)
    return kind, header + exact(length)


def main():
    tag = "macos" if platform.system() == "Darwin" else "linux"
    binary = str(Path(os.environ.get("QTC_BIN", f"build/qtc-{tag}-{platform.machine()}")).resolve())
    with tempfile.TemporaryDirectory(prefix="qtc-status-", dir="/tmp") as tmp:
        env = os.environ.copy()
        for key, folder in (("XDG_DATA_HOME", "data"), ("XDG_CONFIG_HOME", "config"),
                            ("XDG_RUNTIME_DIR", "run")):
            env[key] = str(Path(tmp) / folder)
            Path(env[key]).mkdir()
        command = [binary, "--profile", "health"]
        path = str(Path(tmp) / "run/qtc/health/qtc.sock")

        def status():
            return subprocess.run(command + ["status"], env=env, capture_output=True,
                                  text=True, timeout=5)

        stopped = status()
        assert stopped.returncode == 1 and stopped.stdout == "QTC core: stopped\n"
        legacy_frames = []
        for args, mode in ((["--demo"], "demo"),
                           (["--device", "/dev/qtc-nonexistent-status-test"], "radio")):
            core = subprocess.Popen(command + ["core", "--foreground"] + args, env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                for _ in range(100):
                    result = status()
                    if result.returncode == 0:
                        break
                    assert core.poll() is None, "core exited during startup"
                    time.sleep(0.05)
                assert result.returncode == 0, result.stderr
                assert result.stdout.startswith("QTC core: running\n")
                fields = dict(line.split(": ", 1) for line in result.stdout.splitlines())
                assert fields["PID"] == str(core.pid)
                assert fields["Profile"] == "health"
                assert fields["Database"] == str(Path(tmp) / "data/qtc-terminal/health/qtc.db")
                assert fields["Mode"] == mode
                assert fields["Radio"] == ("connected" if mode == "demo" else "disconnected")
                assert fields["Session"] == ("demo" if mode == "demo" else "disconnected")
                assert fields["Device"] == ("not applicable" if mode == "demo" else args[1])
                assert int(fields["Uptime"][:-1]) >= 0 and fields["Uptime"].endswith("s")
                time.sleep(1.1)
                later = dict(line.split(": ", 1) for line in status().stdout.splitlines())
                assert int(later["Uptime"][:-1]) > int(fields["Uptime"][:-1])
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(3)
                    client.connect(path)
                    client.sendall(struct.pack("<IB", 0, 33))
                    first, second = receive(client), receive(client)
                    assert (first[0], second[0]) == (10, 8), "ordinary PING changed"
                    legacy_frames = [first[1], second[1]]
            finally:
                core.terminate()
                core.wait(timeout=5)
            assert status().stdout == "QTC core: stopped\n"

        # Replay an older core's unchanged PING reply to an enhanced client.
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(path)
            server.listen(1)
            server.settimeout(5)
            client = subprocess.Popen(command + ["status"], env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                conn, _ = server.accept()
                with conn:
                    conn.settimeout(3)
                    kind, request = receive(conn)
                    assert kind == 33 and request[5:] == b"\x01"
                    conn.sendall(b"".join(legacy_frames))
                output, errors = client.communicate(timeout=5)
                assert client.returncode == 0, errors
                assert output.startswith("QTC core: running\nMode: radio\nRadio: disconnected\n")
                assert "PID:" not in output and "Database:" not in output
            finally:
                if client.poll() is None:
                    client.kill()
                    client.wait()
    print("status integration test passed")


if __name__ == "__main__":
    main()
