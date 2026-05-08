#!/usr/bin/env bash
# Build the Jarvis firmware via Windows PlatformIO from a WSL working tree.
#
# Why this exists:
#   The project lives on the WSL ext4 filesystem and is normally accessed from
#   Windows via the \\wsl.localhost\... SMB share. Windows PlatformIO running
#   directly against an SMB-mounted path hangs at "Processing cores3..." —
#   reproducible, presumably file-locking related. WSL's own PlatformIO can
#   install platforms but its dep resolver requires Python 3.10+ and Ubuntu
#   20.04 only ships 3.8, so that path is dead too.
#
#   Workaround: copy the build inputs to a Windows-local temp dir, run Windows
#   pio there, and discard the copy. The .pio cache lives in Windows pio's
#   own ~/.platformio so toolchain/lib downloads persist across runs.
#
# Usage:
#   ./scripts/build.sh            # compile (default)
#   ./scripts/build.sh upload     # compile + flash (device must be on COMx)
#   ./scripts/build.sh uploadfs   # build + flash LittleFS image (web UI in data/)
#   ./scripts/build.sh monitor    # serial monitor on the configured port
#   ./scripts/build.sh clean      # wipe Windows pio's .pio for this project
#
# Run from Git Bash (mingw) on Windows. Requires Windows PlatformIO at the
# default location below.

set -euo pipefail

PIO="/c/Users/jarod/.platformio/penv/Scripts/platformio.exe"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMP_DIR="/c/Users/jarod/AppData/Local/Temp/jarvis-build"

if [[ ! -x "$PIO" ]]; then
    echo "ERROR: Windows PlatformIO not found at $PIO" >&2
    echo "Install via VS Code's PlatformIO extension or pip --user." >&2
    exit 1
fi

cmd="${1:-run}"

# Sync source tree to Windows-local temp. Mirror only what pio needs to
# build: src/, platformio.ini, lib/ (when we have one). Don't copy .pio
# (Windows pio has its own cache), .git, docs/, tools/, platform-tools/.
#
# CRITICAL: only refresh source dirs, never delete TEMP_DIR wholesale —
# that wipes .pio/build/ and forces a 100s cold rebuild every time. With
# the cache preserved, incremental builds for one-file changes are ~10s.
sync_sources() {
    mkdir -p "$TEMP_DIR"
    rm -rf "$TEMP_DIR/src" "$TEMP_DIR/lib" "$TEMP_DIR/include" "$TEMP_DIR/data"
    cp -r "$PROJECT_ROOT/src"            "$TEMP_DIR/"
    cp    "$PROJECT_ROOT/platformio.ini" "$TEMP_DIR/"
    if [[ -d "$PROJECT_ROOT/lib"     ]]; then cp -r "$PROJECT_ROOT/lib"     "$TEMP_DIR/"; fi
    if [[ -d "$PROJECT_ROOT/include" ]]; then cp -r "$PROJECT_ROOT/include" "$TEMP_DIR/"; fi
    # data/ holds the LittleFS web-UI sources packed by `pio run -t uploadfs`.
    # Sync it on every run so a normal `upload` build doesn't drift behind.
    if [[ -d "$PROJECT_ROOT/data"    ]]; then cp -r "$PROJECT_ROOT/data"    "$TEMP_DIR/"; fi
}

case "$cmd" in
    run|build|compile)
        sync_sources
        cd "$TEMP_DIR" && "$PIO" run -e cores3
        ;;
    upload|flash)
        sync_sources
        cd "$TEMP_DIR" && "$PIO" run -e cores3 -t upload
        ;;
    uploadfs|fs)
        # Pack data/ into a LittleFS image and flash to the FS partition.
        # Firmware is unaffected; this only updates the captive-portal web
        # assets. Run after editing data/web/* or the partition layout.
        sync_sources
        cd "$TEMP_DIR" && "$PIO" run -e cores3 -t uploadfs
        ;;
    monitor)
        # No source sync needed; just attach to the device.
        cd "$TEMP_DIR" 2>/dev/null || cd "$PROJECT_ROOT"
        "$PIO" device monitor
        ;;
    clean)
        # Full nuke — useful when libdeps drift or build state is corrupt.
        # Day-to-day rebuilds should never need this.
        rm -rf "$TEMP_DIR"
        echo "Cleaned $TEMP_DIR (next build will be cold, ~100s)"
        ;;
    *)
        echo "Usage: $0 [run|upload|uploadfs|monitor|clean]" >&2
        exit 2
        ;;
esac
