#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

VERSION=${VERSION:-1.3.0}
ARCH=$(uname -m)
EXECUTABLE_NAME="qtc-linux-${ARCH}"
BIN=${BIN:-build/$EXECUTABLE_NAME}
CC=${CC:-cc}
CFLAGS=${CFLAGS:-unknown}
LDFLAGS=${LDFLAGS:-unknown}
DIST=${DIST:-dist}
SOURCE_NAME="qtc-terminal-${VERSION}-source"
BINARY_NAME="qtc-terminal-${VERSION}-linux-${ARCH}"

[[ -x "$BIN" ]] || { echo "missing executable: $BIN" >&2; exit 1; }
actual_version=$("$BIN" --version | awk '{print $2}')
[[ "$actual_version" == "$VERSION" ]] || { echo "binary version $actual_version does not match package version $VERSION" >&2; exit 1; }

git_commit="uncommitted-source"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git_commit=$(git rev-parse HEAD)
fi

if [[ -n "${SOURCE_DATE_EPOCH:-}" ]]; then
    epoch=$SOURCE_DATE_EPOCH
elif git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    epoch=$(git show -s --format=%ct HEAD)
else
    epoch=$(date +%s)
fi

compiler=$($CC --version 2>/dev/null | head -n 1 || true)
target=$($CC -dumpmachine 2>/dev/null || echo unknown)
linker=$(ld --version 2>/dev/null | head -n 1 || echo unknown)
sqlite_version=$(pkg-config --modversion sqlite3 2>/dev/null || echo unknown)
build_date=$(date -u -d "@$epoch" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || date -u '+%Y-%m-%dT%H:%M:%SZ')

rm -rf "$DIST"
mkdir -p "$DIST" "$DIST/.staging/$SOURCE_NAME" "$DIST/.staging/$BINARY_NAME"

manifest="$DIST/RELEASE-MANIFEST.txt"
cat > "$manifest" <<MANIFEST
QTC Terminal release manifest
Version: $VERSION
Git commit: $git_commit
Compiler: ${compiler:-unknown}
Target: $target
Linker: $linker
CFLAGS: $CFLAGS
LDFLAGS: $LDFLAGS
Build date (UTC): $build_date
SOURCE_DATE_EPOCH: $epoch
SQLite development version: $sqlite_version
Database schema: 10
MeshCore target protocol: Companion Protocol v3
MANIFEST

# Copy and test the exact standalone executable that will be released.
install -m 0755 "$BIN" "$DIST/$EXECUTABLE_NAME"
QTC_BIN="$ROOT/$DIST/$EXECUTABLE_NAME" ./tests/demo_core_test.sh

# Package the exact source tree without generated objects, release outputs, or Git metadata.
tar \
    --exclude='./build' \
    --exclude='./dist' \
    --exclude='./.git' \
    --exclude='./src/*.o' \
    --exclude='./src/*.d' \
    --exclude='*.core' \
    -cf - . | tar -xf - -C "$DIST/.staging/$SOURCE_NAME"
cp "$manifest" "$DIST/.staging/$SOURCE_NAME/RELEASE-MANIFEST.txt"

# Keep the binary package focused on installation, usage, build information, licensing, and privacy.
install -m 0755 "$DIST/$EXECUTABLE_NAME" "$DIST/.staging/$BINARY_NAME/$EXECUTABLE_NAME"
for file in README.md CHANGELOG.md BUILDING.md LICENSE NOTICE.md PRIVACY.md; do
    cp "$file" "$DIST/.staging/$BINARY_NAME/$file"
done
cp "$manifest" "$DIST/.staging/$BINARY_NAME/RELEASE-MANIFEST.txt"
mkdir -p "$DIST/.staging/$BINARY_NAME/packaging/systemd"
install -m 0644 packaging/systemd/qtc.service.example "$DIST/.staging/$BINARY_NAME/packaging/systemd/qtc.service.example"

# Normalize archive ownership, ordering, and timestamps.
tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    -czf "$DIST/${SOURCE_NAME}.tar.gz" -C "$DIST/.staging" "$SOURCE_NAME"
tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    -czf "$DIST/${BINARY_NAME}.tar.gz" -C "$DIST/.staging" "$BINARY_NAME"

cp CHANGELOG.md "$DIST/CHANGELOG.md"
cp BUILDING.md "$DIST/BUILDING.md"

(
    cd "$DIST"
    sha256sum \
        "$EXECUTABLE_NAME" \
        "${BINARY_NAME}.tar.gz" \
        "${SOURCE_NAME}.tar.gz" \
        CHANGELOG.md \
        BUILDING.md \
        RELEASE-MANIFEST.txt > SHA256SUMS
    sha256sum -c SHA256SUMS
)

rm -rf "$DIST/.staging"
printf 'Release artifacts created in %s/%s\n' "$ROOT" "$DIST"
