#!/usr/bin/env bash
# Builds a self-contained folder (dist/VideoEditor) for an MSYS2 UCRT64 build:
# copies the exe, runs windeployqt6 for Qt plugins, then copies every other
# DLL the exe depends on (FFmpeg, SQLite, libstdc++, ...) from /ucrt64/bin.
#
# usage (from an MSYS2 UCRT64 shell):  scripts/deploy-windows.sh [build/release]
set -euo pipefail

BUILD_DIR="${1:-build/release}"
DIST="dist/VideoEditor"
MSYS_BIN="${MSYS_BIN:-/ucrt64/bin}"
[ -d "$MSYS_BIN" ] || MSYS_BIN="/c/msys64/ucrt64/bin"
export PATH="$MSYS_BIN:$PATH"

# Files of a copy that is running from dist cannot always be removed; overwrite them instead.
rm -rf "$DIST" 2>/dev/null || echo "note: some files in $DIST are in use (is the editor running from there?); overwriting"
mkdir -p "$DIST"
cp "$BUILD_DIR/VideoEditor.exe" "$BUILD_DIR/VideoEditorMCP.exe" "$DIST/"

windeployqt6 --no-translations --compiler-runtime "$DIST/VideoEditor.exe" >/dev/null 2>&1

# Copy remaining non-system DLL dependencies (recursively, via ldd).
copy_deps() {
    { ldd "$1" | awk '{print $3}' | grep -i "ucrt64/bin" || true; } | while read -r dll; do
        local name
        name="$(basename "$dll")"
        if [ ! -f "$DIST/$name" ]; then
            cp "$dll" "$DIST/"
            copy_deps "$dll"
        fi
    done
}
copy_deps "$DIST/VideoEditor.exe"
copy_deps "$DIST/VideoEditorMCP.exe"
for plugin in "$DIST"/*/*.dll; do copy_deps "$plugin"; done

# Voice/music separation script (its Python environment is set up on first use)
mkdir -p "$DIST/separator"
cp "$(cd "$(dirname "$0")/.." && pwd)/separator/separate.py" "$(cd "$(dirname "$0")/.." && pwd)/separator/transcribe.py" "$DIST/separator/"

# Claude panel: the bridge script + Claude Agent SDK. The SDK's optional
# native Claude Code binary is skipped; the bridge then uses the user's own
# `claude` CLI (and its login).
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
if command -v npm >/dev/null 2>&1 || [ -x "/c/Program Files/nodejs/npm" ]; then
    NPM="$(command -v npm || echo "/c/Program Files/nodejs/npm")"
    mkdir -p "$DIST/claude-bridge"
    cp "$SRC_DIR/claude-bridge/bridge.mjs" "$SRC_DIR/claude-bridge/package.json" "$DIST/claude-bridge/"
    [ -f "$SRC_DIR/claude-bridge/package-lock.json" ] && cp "$SRC_DIR/claude-bridge/package-lock.json" "$DIST/claude-bridge/"
    (cd "$DIST/claude-bridge" && PATH="/c/Program Files/nodejs:$PATH" "$NPM" install --omit=optional --no-audit --no-fund >/dev/null 2>&1) \
        || echo "warning: npm install failed in $DIST/claude-bridge (the Claude panel will ask for it)"
else
    echo "warning: npm not found; the Claude panel needs claude-bridge/ (run npm install there)"
fi

echo "Deployed to $DIST ($(ls "$DIST" | wc -l) entries, $(du -sh "$DIST" | cut -f1))"
