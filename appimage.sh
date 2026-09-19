#!/bin/sh
# Build isf-ARCH.AppImage: put the binary in AppDir, and pack AppDir with
# appimagetool (downloaded once into .cache/).
#
#   ./appimage.sh [BINARY]    default: ./isf, built with make if missing
#
# ARCH defaults to this machine's (uname -m).

set -eu
cd "$(dirname "$0")"
arch=${ARCH:-$(uname -m)}
bin=${1:-isf}

[ -e "$bin" ] || make
install -m 755 "$bin" AppDir/usr/bin/isf

# The tool is an AppImage too. Extracted, it runs without FUSE (CI, containers)
tool=.cache/appimagetool-$arch
if [ ! -x "$tool/AppRun" ]; then
        mkdir -p .cache
        curl -fsSL -o "$tool.AppImage" \
                "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$arch.AppImage"
        chmod +x "$tool.AppImage"
        rm -rf "$tool" .cache/squashfs-root
        (cd .cache && "./appimagetool-$arch.AppImage" --appimage-extract >/dev/null)
        mv .cache/squashfs-root "$tool"
        rm "$tool.AppImage"
fi

ARCH=$arch "$tool/AppRun" --no-appstream AppDir "isf-$arch.AppImage"
