# Changelog

## v1.2.0

### Added / Improved

- Select logical messages in history and press `r` to reply with interoperable
  `@[Name]` text.
- Add `@` autocomplete and incoming mention highlighting, supporting Unicode and
  emoji participant names in the reply/mention workflow.
- Auto-scroll the F7 Network Nodes list to keep selection visible.
- Press Enter in F7 to inspect cached node metadata in a read-only detail view;
  Escape returns to the list with the same node selected.
- Align F7 list columns and truncate long ASCII and Unicode names by display width.

### Compatibility / Notes

- Replies and mentions remain ordinary readable MeshCore text; no proprietary
  on-air metadata is introduced.

## v1.1.1

- Fixed TUI divider misalignment on rows containing wide Unicode / emoji characters.
- Re-anchor the terminal cursor after width-sensitive multibyte framebuffer cells
  while preserving efficient sequential ASCII rendering.

## v1.1.0

- Add documented native Linux ARM64 / aarch64 build support and architecture-specific
  release packaging, including `qtc-linux-aarch64`.
- Add an optional systemd user-service template and headless Linux deployment
  instructions for boot-time startup with `qtc core --foreground`.
- Document stable `/dev/serial/by-id/...` device paths, serial permissions, user
  lingering, and service management.
- Enhance `qtc status` with core PID, uptime, profile, device path, MeshCore session
  state, database path, and node name and firmware when known. Older compatible
  cores continue to provide basic status.
