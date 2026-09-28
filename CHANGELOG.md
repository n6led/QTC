# Changelog

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
