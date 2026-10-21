#!/usr/bin/env bash
# Builds HumHouse-Studio-Linux.deb and HumHouse-Studio-Master-Linux.deb (Debian / Ubuntu / Chromebook Linux)
set -euo pipefail
BUILD="${1:-build}"; OUT="${2:-artifacts}"
VERSION="1.3.2"
mkdir -p "$OUT"
make_deb () {
  local app="$1" pkg="$2" file="$3"
  local bin="$BUILD/${4}_artefacts/Release/$app"
  local root; root="$(mktemp -d)"; chmod 755 "$root"
  mkdir -p "$root/DEBIAN" "$root/opt/$pkg" "$root/usr/bin" "$root/usr/share/applications" "$root/usr/share/doc/$pkg"
  cp "$bin" "$root/opt/$pkg/$app"
  chmod 755 "$root/opt/$pkg/$app"
  cp -R Sounds "$root/opt/$pkg/Sounds"   # bundled kits (the app looks next to itself)
  ln -s "/opt/$pkg/$app" "$root/usr/bin/$pkg"
  cp installer/LICENSE.txt "$root/usr/share/doc/$pkg/copyright"
  cat > "$root/usr/share/applications/$pkg.desktop" <<EOF2
[Desktop Entry]
Type=Application
Name=$app
Comment=Beat making and vocal recording
Exec="/opt/$pkg/$app"
Terminal=false
Categories=AudioVideo;Audio;Music;
EOF2
  cat > "$root/DEBIAN/control" <<EOF2
Package: $pkg
Version: $VERSION
Section: sound
Priority: optional
Architecture: amd64
Depends: libasound2, libfreetype6, libfontconfig1, libx11-6, libxext6, libxinerama1, libxrandr2, libxcursor1, libgl1
Maintainer: Hum House <support@humhouse.studio>
Description: $app - beat making and vocal recording DAW
EOF2
  dpkg-deb --root-owner-group --build "$root" "$OUT/$file"
  rm -rf "$root"
}
make_deb "HumHouse Studio" humhouse-studio HumHouse-Studio-Linux.deb HumHouseStudio
make_deb "HumHouse Studio Master" humhouse-studio-master HumHouse-Studio-Master-Linux.deb HumHouseStudioMaster
ls -la "$OUT"
