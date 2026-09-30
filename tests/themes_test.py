#!/usr/bin/env python3
"""CLI theme selection, real ANSI output, persistence and help/resize over a PTY."""
import fcntl
import os
from pathlib import Path
import pty
import signal
import sqlite3
import struct
import subprocess
import tempfile
import termios
import time

from tui_smoke_test import read_available, render_terminal


def main():
    binary = str(Path(os.environ["QTC_BIN"]).resolve())
    with tempfile.TemporaryDirectory(prefix="qtc-themes-", dir="/tmp") as tmp:
        env = os.environ.copy()
        for key in ("XDG_RUNTIME_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME"):
            env[key] = tmp + "/" + key
            os.mkdir(env[key])
        command = [binary, "--profile", "themes"]
        for args in (["--theme", "invalid"], ["--theme"]):
            result = subprocess.run(command + args, env=env, capture_output=True, text=True, timeout=5)
            assert result.returncode == 2, (result.returncode, result.stdout, result.stderr)
            assert "Invalid theme" in result.stderr and "classic" in result.stderr
        core = subprocess.Popen(command + ["core", "--foreground", "--demo"], env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(50):
                if subprocess.run(command + ["status"], env=env, capture_output=True).returncode == 0:
                    break
                time.sleep(0.1)
            baseline = None
            palettes = {"signal": b"\x1b[1;97;44m", "amber": b"\x1b[1;30;43m",
                        "phosphor": b"\x1b[1;30;42m", "high-contrast": b"\x1b[1;30;107m",
                        "classic": b"\x1b[1;30;42m"}
            for theme, sgr in palettes.items():
                master, slave = pty.openpty()
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 28, 100, 0, 0))
                proc = subprocess.Popen(command + ["--theme", theme], env=env,
                                        stdin=slave, stdout=slave, stderr=slave)
                os.close(slave)
                try:
                    output = read_available(master, 1)
                    assert proc.poll() is None, (theme, proc.returncode, output)
                    assert sgr in output, theme
                    screen = render_terminal(output)
                    assert "DEMO" in screen[0]
                    if baseline is None:
                        baseline = screen
                    else:
                        assert screen == baseline, theme
                    os.write(master, b"?")
                    assert "KEYBOARD HELP" in "\n".join(render_terminal(read_available(master, 0.3)))
                    os.write(master, b"?")
                    assert render_terminal(read_available(master, 0.3)) == baseline
                    os.write(master, b"\x1b[14~")
                    assert f"Theme [t]: {theme}" in "\n".join(render_terminal(read_available(master, 0.3)))
                    if theme == "signal":
                        # Persist the highest new ID using the existing settings IPC.
                        os.write(master, b"t7")
                        read_available(master, 0.3)
                        dbpath = Path(env["XDG_DATA_HOME"]) / "qtc-terminal/themes/qtc.db"
                        with sqlite3.connect(dbpath) as db:
                            assert db.execute("SELECT value FROM settings WHERE key='theme'").fetchone() == ("6",)
                    for rows, cols in ((18, 68), (18, 80), (16, 60), (28, 100)):
                        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
                        proc.send_signal(signal.SIGWINCH)
                        resized = read_available(master, 0.2)
                        assert proc.poll() is None
                        if cols < 68:
                            assert b"Terminal is too small" in resized
                    os.write(master, b"\x1b[19~")
                    proc.wait(timeout=5)
                    assert proc.returncode == 0
                finally:
                    if proc.poll() is None:
                        proc.terminate(); proc.wait(timeout=5)
                    os.close(master)
            print("theme CLI/ANSI/persistence/help/resize integration tests passed")
        finally:
            core.terminate(); core.wait(timeout=5)


if __name__ == "__main__":
    main()
