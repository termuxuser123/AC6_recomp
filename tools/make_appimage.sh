#!/usr/bin/env bash
# Package the native Linux build as a single-file AppImage.
#
# Usage (from the repo root, after a successful Linux build):
#   tools/make_appimage.sh
# Output: dist/ac6recomp-x86_64.AppImage
#
# The AppImage bundles the libraries the game links against (GTK3 and its
# dependencies, via linuxdeploy's GTK plugin). It does not bundle glibc, the
# Vulkan loader or GPU drivers - those always come from the user's system -
# so build on the oldest distro you want to support.
#
# At runtime the game treats the folder containing the .AppImage file as its
# own folder: put assets/ (or the .iso) next to the .AppImage, and the config
# and logs are written there too.
#
# Environment overrides: BUILD_DIR (default out/build/linux-amd64-relwithdebinfo),
# BIN (default $BUILD_DIR/ac6recomp), OUT_DIR (default dist).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/out/build/linux-amd64-relwithdebinfo}"
BIN="${BIN:-$BUILD_DIR/ac6recomp}"
OUT_DIR="${OUT_DIR:-$ROOT/dist}"
TOOLS_DIR="$ROOT/out/appimage-tools"
APPDIR="$ROOT/out/AppDir"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not found - build the game first (see README, Building on Linux)" >&2
  exit 1
fi

mkdir -p "$TOOLS_DIR" "$OUT_DIR"

fetch() {  # fetch <url> <dest>
  if [[ ! -s "$2" ]]; then
    echo "Downloading $(basename "$2")..."
    curl -fL --retry 3 -o "$2.part" "$1"
    mv "$2.part" "$2"
  fi
  chmod +x "$2"
}
fetch https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
      "$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
fetch https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh \
      "$TOOLS_DIR/linuxdeploy-plugin-gtk.sh"

# Fresh AppDir with only what ships: the binary and the controller mappings
# (the input driver loads gamecontrollerdb.txt from next to the executable,
# which inside the AppImage is usr/bin).
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
cp "$BIN" "$APPDIR/usr/bin/ac6recomp"
strip "$APPDIR/usr/bin/ac6recomp" || true
cp "$ROOT/gamecontrollerdb.txt" "$APPDIR/usr/bin/"

cat > "$OUT_DIR/ac6recomp.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=AC6 Recomp
Comment=Ace Combat 6 static recompilation
Exec=ac6recomp
Icon=ac6recomp
Categories=Game;
Terminal=false
EOF

# linuxdeploy requires an icon; generate a plain 256x256 one (no game art).
python3 - "$OUT_DIR/ac6recomp.png" <<'EOF'
import struct, sys, zlib
w = h = 256
row = b"\x00" + bytes((0x1f, 0x3a, 0x5f)) * w
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(row * h, 9)) + chunk(b"IEND", b""))
open(sys.argv[1], "wb").write(png)
EOF

# APPIMAGE_EXTRACT_AND_RUN: run linuxdeploy without needing FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1
export DEPLOY_GTK_VERSION=3
export OUTPUT="$OUT_DIR/ac6recomp-x86_64.AppImage"
export PATH="$TOOLS_DIR:$PATH"
rm -f "$OUTPUT"

"$TOOLS_DIR/linuxdeploy-x86_64.AppImage" \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/ac6recomp" \
  --desktop-file "$OUT_DIR/ac6recomp.desktop" \
  --icon-file "$OUT_DIR/ac6recomp.png" \
  --plugin gtk \
  --output appimage

rm -f "$OUT_DIR/ac6recomp.desktop" "$OUT_DIR/ac6recomp.png"
echo
echo "Created $OUTPUT"
echo "Put assets/ (or your .iso) next to it and run: ./$(basename "$OUTPUT")"
