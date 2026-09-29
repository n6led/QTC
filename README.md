# QTC Terminal

**An old-school-cool terminal client for MeshCore messaging on Linux and macOS.**

> This repository is a fork of [initsixdev/QTC](https://github.com/initsixdev/QTC), focused on Linux ARM64, headless Raspberry Pi deployments, and MeshCore USB Companion operation.

<p align="center">
  <img src="docs/qtc.png" alt="QTC Terminal" width="900">
</p>

QTC turns a USB-connected MeshCore Companion radio into a desktop terminal messenger. It provides direct messages, channels, local history, favorites, notifications, and a persistent background connection without requiring a graphical desktop client.

QTC is local-first: your message history and settings stay on your machine, and ordinary messaging goes through your MeshCore radio.

## Quick start

### Linux

QTC requires a MeshCore-compatible device connected over USB.

The device will usually appear as:

```sh
/dev/ttyACM0
```

or:

```sh
/dev/ttyUSB0
```

Check that it is detected:

```sh
ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

On most Linux distributions, your user must be a member of the dialout group to access USB serial devices:

```sh
sudo usermod -aG dialout "$USER"
```

Then log out completely and log back in for the new group membership to take effect.

Verify with:

```sh
groups
```

dialout should appear in the list.

Reconnect the MeshCore device and run QTC normally. Do not run QTC with sudo.

If your distribution does not use the dialout group, check which group owns the serial device:

```sh
ls -l /dev/ttyACM0
```

and add your user to that group instead.

Connect the MeshCore device before starting QTC. QTC should run as your normal user; sudo is not normally required.
On x86-64 Linux, download `qtc-linux-x86_64` from the [latest GitHub release](https://github.com/initsixdev/QTC/releases/latest), then:

```sh
chmod +x qtc-linux-x86_64
sudo install -m 0755 qtc-linux-x86_64 /usr/local/bin/qtc
qtc
```

On ARM64 Linux (`uname -m` reports `aarch64`), build natively from source.
From the source directory on Debian or Ubuntu, including Debian 13 on Raspberry Pi 4:

```sh
sudo apt install build-essential libsqlite3-dev python3
make
make test
sudo make install
qtc
```

This produces `build/qtc-linux-aarch64`. See [BUILDING.md](BUILDING.md) for
the reported ARM64 hardware validation and packaging instructions.

### macOS

There is no prebuilt macOS binary, so build from source. Nothing beyond Apple's toolchain is required, because the macOS SDK already supplies SQLite 3:

```sh
xcode-select --install
make
sudo make install
qtc
```

### Choosing a radio

```sh
qtc --list-devices
qtc --device /dev/ttyACM0            # Linux
qtc --device /dev/cu.usbmodem1101    # macOS
```

Automatic detection looks for `/dev/serial/by-id/*`, `/dev/ttyACM*`, and `/dev/ttyUSB*` on Linux, and for `/dev/cu.usbmodem*`, `/dev/cu.usbserial*`, `/dev/cu.wchusbserial*`, and `/dev/cu.SLAB_USBtoUART*` on macOS. Bluetooth and debug serial ports are never offered.

macOS needs no permission step: a connected radio's `/dev/cu.*` node already belongs to the logged-in user. The matching `/dev/tty.*` node is deliberately ignored, because it blocks on carrier detect and is the wrong device for a modem-style radio. Some USB-serial bridges need their vendor driver installed before the node appears at all.

If Linux denies access to the serial device, add your user to the serial-access group used by your distribution:

```sh
sudo usermod -aG dialout "$USER"
```

Then log out completely and log back in. QTC can also print a udev rule:

```sh
qtc --print-udev-rule | sudo tee /etc/udev/rules.d/99-qtc-meshcore.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug and reconnect the radio after installing the rule. On macOS the same command prints the device notes for that platform instead.

## Features

- Direct MeshCore messaging with local conversation history.
- Channel messaging and private-channel management.
- Join a private channel from an invitation URI, a raw 32-character key, or `Name:key`.
- Invite selected MeshCore contacts to private channels with an explicit confirmation screen.
- Contact aliases, favorites, favorite groups, unread state, and search.
- Contacts grouped by route distance, with infrastructure nodes separated from people.
- Persistent background core: detach the terminal without disconnecting the radio.
- Desktop notifications and notification sounds.
- Four built-in terminal themes: Green Phosphor, Amber CRT, Midnight BBS, and Mono TTY.
- Wrapped message history with Page Up/Page Down navigation.
- Radio controls for name, TX power, synchronization, reconnect, and advertisements.
- Zero-hop and flood self advertisements with visible feedback.
- Export your radio's MeshCore contact card to the clipboard.
- First-connect regional radio presets.
- Long UTF-8 messages are split and reassembled automatically between QTC clients.

## Main controls

| Key | Action |
|---|---|
| `Up` / `Down`, `j` / `k` | Move selection |
| `Enter` | Open the selected conversation and start typing |
| `m` | Compose in the open conversation |
| `f` | Toggle favorite |
| `F2` / `e` | Edit contact alias |
| `g` | Set favorite group |
| `/` | Search |
| `Page Up` / `Page Down` | Scroll conversation history |
| `Tab` | Focus logical messages in an open conversation; press again for roster |
| `Up` / `Down`, `j` / `k` in message selection | Select previous/next logical message |
| `r` in message selection | Reply with a sender mention; does not send |
| `F4` / `s` | Settings |
| `F5` | Reconnect radio |
| `F6` | Channels |
| `F7` | Network Nodes |
| `F8` / `Ctrl+C` | Detach this terminal; keep QTC running |
| `Ctrl+Q` twice | Stop QTC completely and release USB |
| `Esc` | Cancel or return |

### Replies and mentions

Open a conversation, then press `Tab` to select messages. The selected message's
metadata is highlighted and its text has a `>` marker. Up/Down or j/k moves one
logical message; Page Up/Down jumps five messages. Existing multipart groups are
one selectable item. Selection follows the local logical key across new arrivals
and resizing; if that message leaves the in-memory history, a visible message is
selected instead. Tab or Escape returns to roster navigation. `m` resumes writing.

Press `r` on an incoming message with a known sender to seed the composer with
`@[Sender] `, including a trailing space. For example, a reply to MeshGarden BOT
starts `@[MeshGarden BOT] `. Nothing is sent until you press Enter in the composer.
No quotation or local message identifier is added. Outbound messages and unknown
senders do not offer Reply; use `m` to write normally.

Direct replies use the known contact's display name (local alias if set). Channel
replies recognize the conventional `Name: ` prefix, not arbitrary colon-separated
text. These channel names are unverified display information. See
[protocol notes](docs/PROTOCOL-NOTES.md#reply-and-mention-text) for the exact rule.

Typing `@` at the beginning of the composer or after a space opens suggestions.
Continue typing to filter using the existing roster search rules, use Up/Down to
choose, and Enter to insert `@[Exact Display Name] ` without sending. Escape closes
suggestions and keeps what you typed. Typing `[` closes suggestions so you can
enter a mention manually. Ordinary text such as `mail@example.com` stays ordinary
text. Candidates are deduplicated: recent incoming senders in this conversation,
the direct contact, then known people, with a maximum of 64 matching names. No
participant database is created. Names with spaces, hyphens, numbers, Unicode, and
emoji are preserved; malformed UTF-8, control characters, and names containing
`[` or `]` are excluded. Complete `@[Name]` mentions use the theme's accent style,
including when wrapped, without altering stored text or notification behavior.

A single draft is kept in memory for the open conversation. Escape from ordinary
composition saves it; `m` resumes it. Reply prepends a mention to that draft rather
than replacing it. Escape from reply composition restores the pre-reply draft;
Tab or switching views keeps the edited reply as the draft. To prevent accidental
loss, opening a different conversation is blocked while a draft exists: resume it,
send it, or erase its contents and press Enter to discard it. Drafts are not saved
across TUI detach or process exit.

Replies remain ordinary MeshCore text, for example `@[N6LED] Yes, that worked.`
The existing message-size and multipart handling applies unchanged. No new packet
type, reply ID, private header, database schema, or IPC payload is introduced.

### Channels

| Key | Action |
|---|---|
| `c` | Create private channel |
| `j` | Join from URI, raw key, or `Name:key` |
| `i` | Invite one or more contacts |
| `r` | Rotate private-channel key |
| `d` | Leave channel while keeping local history |
| `v` | Review a pending invitation |

### Settings

| Key | Action |
|---|---|
| `t` | Choose theme |
| `d` | Change radio name |
| `p` | Change TX power |
| `z` | Send zero-hop advertisement |
| `x` | Send flood advertisement |
| `c` | Copy your MeshCore contact card |
| `o` | Open regional radio presets |
| `y` | Force stored-message synchronization |
| `r` / `F5` | Reconnect radio |
| `n` | Test desktop notification |
| `a` | Test notification sound |

Additional notification, retry, display, and history options are available directly in the Settings screen.

## Background operation

QTC uses one background core per profile. The core owns the USB connection and local database while the terminal UI is only a client of that core.

This means:

- `Ctrl+C` or `F8` closes the terminal UI but keeps the radio connected.
- Messages continue to be received and stored while no terminal is attached.
- Desktop notifications and sounds can continue in the background.
- Running `qtc` again reconnects to the existing core.
- `qtc shutdown` or `Ctrl+Q` twice stops the core and releases USB.

Check the running core with:

```sh
qtc status
```

Status queries the core through local IPC without opening the radio or querying
the database. It reports PID, uptime in whole seconds, profile, mode, radio
connection, device, MeshCore session phase, and database path. Node name and
firmware are shown when known (they may be last-known values after a disconnect).
`Device` is the active serial path while open, otherwise the configured path;
it is `unknown` when none is known and `not applicable` in demo mode.
`Session: ready` describes handshake completion, while `Status` is the latest
core status message. Neither guarantees radio reachability at this instant.

Uptime uses a monotonic clock since core initialization and resets on restart.
The first line remains `QTC core: running` or `QTC core: stopped`; stopped cores
emit no runtime fields and return exit status 1. Incompatible cores report running
with an error on stderr and a nonzero exit status. Older compatible cores return
the original basic status without the new runtime fields. RX/TX ages and database
totals are not collected by this command.

### Start at boot with a systemd user service (Linux)

QTC's detached core survives TUI and SSH disconnects, but does not start itself
after a full reboot. On headless Linux systems, use the supplied
[`qtc.service.example`](packaging/systemd/qtc.service.example) to start the core
with the systemd user manager. The template is included in source and Linux binary
release archives; installation is optional and manual. It runs QTC as your normal
user with `core --foreground`, using the default profile.

First install QTC at `/usr/local/bin/qtc` (see [BUILDING.md](BUILDING.md)) and find
your radio's stable path:

```sh
qtc --list-devices
ls -l /dev/serial/by-id/
groups
```

Prefer `/dev/serial/by-id/...` over `/dev/ttyACM0`, whose number can change.
On Debian-family systems, if your user lacks serial access:

```sh
sudo usermod -aG dialout "$USER"
```

Log out completely and log back in, then check `groups` again. A lingering user
manager may retain its old group membership; reboot after changing groups if
the service still reports permission denied. On other distributions, use the
group that owns the serial device. Do not run QTC or `systemctl --user` with sudo.

From the source tree or extracted binary release directory:

```sh
mkdir -p ~/.config/systemd/user
cp packaging/systemd/qtc.service.example ~/.config/systemd/user/qtc.service
${EDITOR:-nano} ~/.config/systemd/user/qtc.service
```

Replace `REPLACE_WITH_YOUR_DEVICE` in `ExecStart` with your radio's actual by-id
name. Adjust the executable path if QTC is installed elsewhere. For example, the
reported Tracker L1 Pro deployment uses this device path (your identifier will
differ):

```text
/dev/serial/by-id/usb-Seeed_Studio_Seeed_Wio_Tracker_L1_4BE7EE8B1A4E4859-if00
```

The service uses the normal user's default data locations. If you use a custom
`--profile` or XDG data/config paths, configure the same values in the service
and your interactive commands; a user service does not read your shell startup
files. Stop any existing detached core for that profile with `qtc shutdown`
before enabling the service, and wait for `qtc status` to report it stopped.
Do not launch the TUI again until the service is running.

Enable lingering so the user manager starts at boot without an interactive login
and remains available after logout. Run this as the account that will run QTC
(`$USER` supplies the username; equivalently use `sudo loginctl enable-linger <username>`):

```sh
sudo loginctl enable-linger "$USER"
systemctl --user daemon-reload
systemctl --user enable --now qtc.service
systemctl --user status qtc.service
qtc status
```

Check both statuses: an active service means the core process is running, while
`qtc status` should report `Mode: radio`, `Radio: connected`, and
`Status: MeshCore session ready` once the radio is ready. Inspect logs with:

```sh
journalctl --user -u qtc.service -b
```

`Restart=on-failure` and `RestartSec=5` retry process failures after five seconds.
When USB is temporarily absent, the current core normally stays alive and retries
the radio connection itself; systemd does not restart a still-running process.
Neither mechanism fixes an incorrect device path or missing serial permissions.
If systemd reports a start-limit failure, fix the cause, run
`systemctl --user reset-failed qtc.service`, then restart it.

To manage the service:

```sh
systemctl --user restart qtc.service
systemctl --user stop qtc.service
```

After editing the unit, run `systemctl --user daemon-reload` before restarting.
Running `qtc` attaches the TUI to the service's core; F8 detaches it and reception
continues after SSH logout. `qtc shutdown` or Ctrl+Q twice exits cleanly and is
not restarted by `Restart=on-failure`; use `systemctl --user restart qtc.service`
to resume. Starting `qtc` while the service is stopped can create a detached core
again, so start the service first.

To disable boot startup and remove the service:

```sh
systemctl --user disable --now qtc.service
rm ~/.config/systemd/user/qtc.service
systemctl --user daemon-reload
```

This preserves QTC's database and settings. If you enabled lingering only for QTC
and no other user services need it, optionally run `sudo loginctl disable-linger "$USER"`.

The manually configured user-service model has been reported working on a
Raspberry Pi 4 running Debian 13 ARM64 with a Seeed Wio Tracker L1 Pro, including
reboot startup and reception without an SSH session. That report does not replace
boot-time validation of this repository template on your Linux deployment.

## Notifications and sound

QTC works without desktop integration. It looks for helper programs at runtime and uses the first one present, so the same build behaves correctly on any desktop. Notifications try `notify-send`, then `terminal-notifier`, then `osascript`. Sound tries `canberra-gtk-play`, then `pw-play`, then `afplay`. Clipboard export tries `wl-copy`, `xclip`, `xsel`, then `pbcopy`.

macOS ships `osascript`, `afplay`, and `pbcopy`, so nothing needs installing. `terminal-notifier` is optional and gives QTC its own notification identity instead of Script Editor's.

On Linux, install the optional helpers for your distribution.

Fedora:

```sh
sudo dnf install libnotify pipewire-utils
```

Debian or Ubuntu:

```sh
sudo apt install libnotify-bin pipewire-bin
```

Arch Linux:

```sh
sudo pacman -S libnotify pipewire-audio
```

Set `QTC_SOUND_FILE` to choose the notification sound:

```sh
export QTC_SOUND_FILE=/System/Library/Sounds/Submarine.aiff
```

## Command line

Common commands:

```sh
qtc status
qtc shutdown
qtc channel list
qtc channel create "Family"
qtc channel join "meshcore://channel/add?name=Family&secret=..."
qtc channel join "00112233445566778899aabbccddeeff"
qtc channel join "Family:00112233445566778899aabbccddeeff"
qtc channel invite "Family" "Ana"
qtc channel rotate "Family"
qtc channel leave "Family"
qtc test-notify
qtc test-sound
```

Global options such as `--profile` and `--device` may be placed before or after a command.

## Build from source

One source tree supports native Linux x86-64, Linux ARM64 (`aarch64`), and macOS
builds. Required development packages:

- C11 compiler
- GNU Make
- SQLite 3 development headers and library
- POSIX development environment
- Bash and Python 3 to run the complete test suite

Fedora:

```sh
sudo dnf install gcc make sqlite-devel
make
make test
```

Debian or Ubuntu:

```sh
sudo apt install build-essential libsqlite3-dev python3
make
make test
```

macOS, using the Xcode Command Line Tools:

```sh
xcode-select --install
make
make test
```

The executable is named for the host platform:

```text
build/qtc-linux-x86_64
build/qtc-linux-aarch64
build/qtc-macos-arm64
```

See [BUILDING.md](BUILDING.md) for sanitizer, release-check, and packaging commands.

## Local data

QTC stores profile data in SQLite under:

```text
${XDG_DATA_HOME:-~/.local/share}/qtc-terminal/<profile>/qtc.db
```

Runtime socket and lock state live under:

```text
$XDG_RUNTIME_DIR/qtc/<profile>/
```

macOS does not define `XDG_RUNTIME_DIR`, so it uses a user-owned directory below `/tmp` instead. The socket is permission-restricted and the core verifies the connecting user's UID on both platforms.

Use `--profile` to keep independent QTC setups on the same machine.

See [PRIVACY.md](PRIVACY.md) for the privacy model.

## Long-message compatibility

Long messages use normal MeshCore text messages with a small QTC multipart envelope. QTC reassembles those parts into one logical message. Other clients that do not understand the QTC multipart format may display the individual parts separately.

## MeshCore interoperability

QTC communicates with MeshCore Companion firmware through the public MeshCore Companion Protocol. QTC is an independent project and is not an official MeshCore client.

Protocol documentation:

- https://docs.meshcore.io/companion_protocol/
- https://github.com/meshcore-dev/MeshCore/wiki/Companion-Radio-Protocol

## License

QTC is open-source software licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).
