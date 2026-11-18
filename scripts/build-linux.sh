#!/usr/bin/env bash
# Builds the Linux AppImage (dist/VideoEditor-<version>-x86_64.AppImage).
#
# Runs inside an Ubuntu 24.04 container so the result works on most current
# distros (glibc 2.39+). From the repository root, on any machine with Docker:
#
#   docker run --rm -v "$PWD:/src" ubuntu:24.04 bash /src/scripts/build-linux.sh
#
# It can also run directly on an Ubuntu 24.04+ machine (needs root for apt).
set -euo pipefail

SRC="${SRC:-/src}"
WORK="${WORK:-/tmp/ve-linux}"
OUT="$SRC/dist"
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config file wget ca-certificates rsync \
    qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev libqt6opengl6-dev qt6-wayland \
    libgl1-mesa-dev libvulkan-dev glslc \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    libsqlite3-dev nodejs npm librsvg2-bin >/dev/null

VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt" | head -1)"

# Build from a copy (the mounted tree may be slow, and its build/ is Windows-only).
mkdir -p "$WORK"
rsync -a --delete --exclude build --exclude dist --exclude .git --exclude node_modules "$SRC/" "$WORK/src/"
cmake -S "$WORK/src" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DVE_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$WORK/build" --parallel

# AppDir: the app looks for separator/ and claude-bridge/ next to its executable.
APPDIR="$WORK/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin/separator" "$APPDIR/usr/bin/claude-bridge"
cp "$WORK/build/VideoEditor" "$WORK/build/VideoEditorMCP" "$APPDIR/usr/bin/"
cp "$SRC"/separator/{separate,transcribe,upscale,tts,translate}.py "$APPDIR/usr/bin/separator/"
cp "$SRC/claude-bridge/bridge.mjs" "$SRC/claude-bridge/package.json" "$APPDIR/usr/bin/claude-bridge/"
[ -f "$SRC/claude-bridge/package-lock.json" ] && cp "$SRC/claude-bridge/package-lock.json" "$APPDIR/usr/bin/claude-bridge/"
(cd "$APPDIR/usr/bin/claude-bridge" && npm install --omit=optional --no-audit --no-fund >/dev/null 2>&1) \
    || echo "warning: npm install failed (the Claude panel will ask for it)"

# linuxdeploy + Qt plugin bundle the Qt/FFmpeg libraries and Qt plugins.
TOOLS="$WORK/tools"
mkdir -p "$TOOLS"
for t in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage appimagetool-x86_64.AppImage; do
    [ -x "$TOOLS/$t" ] && continue
    case "$t" in
        linuxdeploy-x86_64.AppImage) url="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/$t" ;;
        linuxdeploy-plugin-qt-*) url="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/$t" ;;
        appimagetool-*) url="https://github.com/AppImage/appimagetool/releases/download/continuous/$t" ;;
    esac
    wget -q -O "$TOOLS/$t" "$url"
    chmod +x "$TOOLS/$t"
done
export APPIMAGE_EXTRACT_AND_RUN=1          # no FUSE inside containers
export PATH="$TOOLS:$PATH"
export QMAKE=/usr/lib/qt6/bin/qmake
# xcb is deployed by default; offscreen is needed by the headless MCP server, wayland for Wayland desktops.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so;libqwayland-egl.so;libqwayland-generic.so"
export LDAI_OUTPUT="$OUT/VideoEditor-$VERSION-x86_64.AppImage"
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
mkdir -p "$OUT"

cd "$WORK"
mkdir -p "$WORK/icons"   # AppImage's .DirIcon must be a PNG
rsvg-convert -w 256 -h 256 "$SRC/packaging/linux/videoeditor.svg" -o "$WORK/icons/videoeditor.png"
linuxdeploy-x86_64.AppImage --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/VideoEditor" --executable "$APPDIR/usr/bin/VideoEditorMCP" \
    --desktop-file "$SRC/packaging/linux/videoeditor.desktop" \
    --icon-file "$WORK/icons/videoeditor.png" --icon-file "$SRC/packaging/linux/videoeditor.svg" \
    --plugin qt
ARCH=x86_64 appimagetool-x86_64.AppImage --no-appstream "$APPDIR" "$LDAI_OUTPUT"

# Plain tarball as well, for people who prefer not to use AppImage.
tar -C "$WORK" -czf "$OUT/VideoEditor-$VERSION-linux-x86_64.tar.gz" --transform "s|^AppDir|VideoEditor-$VERSION|" AppDir
chown "$(stat -c %u:%g "$SRC")" "$OUT"/VideoEditor-"$VERSION"-* 2>/dev/null || true
ls -la "$OUT"/VideoEditor-"$VERSION"-*
