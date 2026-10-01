# Building QTC Terminal

## Requirements

One source tree supports native executables for Linux x86-64 and ARM64 (`aarch64`)
with glibc, and for macOS. Cross-compilation is not required.

Build requirements:

- C11 compiler (`gcc` or `clang`)
- GNU Make
- SQLite 3 development headers and library
- standard POSIX shell tools
- Bash and Python 3 for the complete test suite (no third-party Python packages)
- `tar` and `sha256sum` for release packaging, which is Linux-only

Fedora:

```sh
sudo dnf install gcc make sqlite-devel
```

Debian or Ubuntu:

```sh
sudo apt install build-essential libsqlite3-dev python3
```

macOS:

```sh
xcode-select --install
```

The macOS SDK supplies SQLite 3, so nothing else is needed.

## Build

```sh
make clean
make
```

The Makefile selects platform settings from `uname` and names the output for the host:

```text
build/qtc-linux-x86_64
build/qtc-linux-aarch64
build/qtc-macos-arm64
```

The default build uses strict warnings and common hardening flags. Compile flags are
shared by both platforms; preprocessor and linker settings differ, because Darwin
hides `flock`, `LOCK_*`, and `MSG_DONTWAIT` behind `_POSIX_C_SOURCE` and the Apple
linker rejects `-Wl,-z,...`:

```text
Linux    -D_POSIX_C_SOURCE=200809L   -Wl,-z,relro,-z,now
macOS    -D_DARWIN_C_SOURCE          -Wl,-dead_strip
```

### Native Linux ARM64

Use a 64-bit Linux installation; `uname -m` should report `aarch64`.
On Debian or Ubuntu ARM64, from the source directory:

```sh
sudo apt install build-essential libsqlite3-dev python3
make clean
make
make test
./build/qtc-linux-aarch64 --version
sudo make install
```

The same Makefile and compiler flags are used on Linux x86-64 and ARM64.
`make install` installs the host's executable as `/usr/local/bin/qtc`.
An ARM-capable board running a 32-bit OS is outside this ARM64 target.

User-reported validation covers Raspberry Pi 4, Debian GNU/Linux 13 (Trixie)
ARM64, and a Seeed Wio Tracker L1 Pro with MeshCore USB Companion firmware:
native build, the full existing test suite, live USB messages, detach/reattach,
and background reception across SSH logout. This repository currently has no
automated CI or release workflows; this hardware report is not CI verification.

## Tests

Run the complete test suite:

```sh
make test
```

The suite covers the database, message deduplication and multipart handling, invitations, IPC, MeshCore framing and command encoding, roster/search behavior, background-core lifecycle, and terminal interaction.

## Sanitizers

```sh
make sanitize
```

This runs the suite with AddressSanitizer and UndefinedBehaviorSanitizer, then rebuilds the normal executable.

The instrumented pass runs with `QTC_TIMING_SCALE=10`. Sanitizers slow the
SQLite-backed inbox path by roughly twenty times, which no latency budget tuned for an
optimized binary can absorb; a 24-message burst drain measures about 6.6 s against a
0.3 s optimized baseline. That pass still proves no message is lost, and the optimized
rebuild that follows it re-runs the same tests at the real budgets.

## Full release check

```sh
make release-check
```

This performs a clean build, tests, sanitizer validation, and a final normal build.

## Package release artifacts

Release packaging runs on Linux only. It depends on GNU `sha256sum`, `tar --sort` and
`--owner`, `date -d`, and `ld --version`, none of which behave the same way on macOS.
`make package` stops with an explanatory error there. A macOS build is produced with
`make` and installed with `make install`.

```sh
make package
```

Packaging is native: run it on Linux x86-64 or ARM64 for that architecture's
artifacts. The script uses `uname -m`, matching the Makefile. A `BIN` override
must still point to an executable for the host architecture.

Artifacts are written to `dist/` (where `<arch>` is `x86_64` or `aarch64`):

- `qtc-linux-<arch>`
- `qtc-terminal-1.3.0-linux-<arch>.tar.gz`
- `qtc-terminal-1.3.0-source.tar.gz`
- `CHANGELOG.md`
- `BUILDING.md`
- `RELEASE-MANIFEST.txt`
- `SHA256SUMS`

`dist/` is generated output and should not be committed to Git.
Each packaging run replaces `dist/`; retain artifacts separately when collecting
releases from multiple hosts.

## Reproducible archive metadata

Set `SOURCE_DATE_EPOCH` to normalize archive timestamps:

```sh
SOURCE_DATE_EPOCH=1786032000 make package
```

When the source is inside a Git repository, the package script uses the current commit timestamp by default. Compiler output can still vary across compiler, linker, libc, and SQLite versions; the generated release manifest records the relevant build inputs.

## Install

System-wide under `/usr/local/bin`:

```sh
sudo make install
```

Staged installation:

```sh
make DESTDIR=/tmp/qtc-package-root install
```

## Alternate compiler

```sh
make clean
make CC=clang
make test CC=clang
```

## Debug logging

For headless Linux deployment, follow the [systemd user-service guide](README.md#start-at-boot-with-a-systemd-user-service-linux)
and configure a stable `/dev/serial/by-id/...` path. The core waits when that
path is absent and retries it every three seconds, including after USB unplug
or reset. It reinitializes the Companion without restarting the core or choosing
another device. Hardware unplug/replug validation remains necessary for your
radio; `make test` covers recovery with simulated serial devices.

Run QTC in demo mode with debug logging:

```sh
./build/qtc-linux-x86_64 --debug --demo
# ARM64 Linux: ./build/qtc-linux-aarch64 --debug --demo
# macOS: ./build/qtc-macos-arm64 --debug --demo
```

When using a real radio, debug output may contain contact names, message metadata, or device paths. Review logs before sharing them publicly.
