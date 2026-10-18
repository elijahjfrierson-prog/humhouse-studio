#!/usr/bin/env bash
# Builds HumHouse-Studio-macOS.pkg: a guided installer that shows the EULA (must be accepted)
# and installs "HumHouse Studio.app" into /Applications.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
VERSION="${VERSION:-1.2.3}"
BUNDLE_ID="com.humhouse.studio.installer"
PKG_OUT="${PKG_OUT:-HumHouse-Studio-macOS.pkg}"
NAME="HumHouse Studio"
APP_SRC="$REPO/build/HumHouseStudio_artefacts/Release/$NAME.app"
[[ -d "$APP_SRC" ]] || { echo "ERROR: $APP_SRC not found" >&2; exit 1; }

STAGE="$(mktemp -d)"
ROOT="$STAGE/root/Applications"
mkdir -p "$ROOT" "$STAGE/pkgs" "$STAGE/scripts" "$STAGE/resources"
cp -R "$APP_SRC" "$ROOT/"
codesign --force --deep --sign "${APPLE_DEVELOPER_ID:--}" "$ROOT/$NAME.app" || true

cat > "$STAGE/scripts/preinstall" <<PRE
#!/bin/sh
# the new version replaces the old one: quit it if running, then remove the old bundle
pkill -x "$NAME" 2>/dev/null || true
sleep 1
rm -rf "/Applications/$NAME.app" 2>/dev/null || true
exit 0
PRE
chmod +x "$STAGE/scripts/preinstall"

# Without this, macOS "relocates" the install onto any existing copy of the bundle id (e.g. the build dir).
pkgbuild --analyze --root "$STAGE/root" "$STAGE/component.plist"
plutil -replace BundleIsRelocatable -bool NO "$STAGE/component.plist"
pkgbuild --identifier "${BUNDLE_ID}.app" --version "$VERSION" --root "$STAGE/root" \
  --component-plist "$STAGE/component.plist" \
  --scripts "$STAGE/scripts" --install-location "/" "$STAGE/pkgs/app.pkg"

cat > "$STAGE/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>$NAME $VERSION</title>
    <welcome    file="welcome.html"    mime-type="text/html"/>
    <license    file="license.txt"     mime-type="text/plain"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <choices-outline><line choice="app"/></choices-outline>
    <choice id="app" title="$NAME" visible="false" start_selected="true"><pkg-ref id="${BUNDLE_ID}.app"/></choice>
    <pkg-ref id="${BUNDLE_ID}.app" version="$VERSION" onConclusion="none">app.pkg</pkg-ref>
</installer-gui-script>
XML

cp "$REPO/installer/LICENSE.txt" "$STAGE/resources/license.txt"
cat > "$STAGE/resources/welcome.html" <<'HTML'
<html><body style="font-family: -apple-system, system-ui; padding: 20px;">
<h2 style="margin-top:0;">Welcome to HumHouse Studio</h2>
<p>Make beats and record vocals: Channel Rack, Piano Roll with scale highlighting, Playlist with
multi-track recording and punch-in, Mixer with click-to-route, and Plugin House for every VST3 / AU you own.</p>
<p>HumHouse Studio will be installed in /Applications.</p>
</body></html>
HTML
cat > "$STAGE/resources/conclusion.html" <<'HTML'
<html><body style="font-family: -apple-system, system-ui; padding: 20px;">
<h2 style="margin-top:0;">Installation complete.</h2>
<p>Open <b>HumHouse Studio</b> from Applications. First time: Options &rarr; Plugin House: scan plugins,
and allow microphone access when asked so you can record.</p>
</body></html>
HTML

UNSIGNED="$STAGE/unsigned.pkg"
productbuild --distribution "$STAGE/distribution.xml" --resources "$STAGE/resources" \
  --package-path "$STAGE/pkgs" --version "$VERSION" "$UNSIGNED"
if [[ -n "${APPLE_INSTALLER_ID:-}" ]]; then productsign --sign "$APPLE_INSTALLER_ID" "$UNSIGNED" "$PKG_OUT"; else cp "$UNSIGNED" "$PKG_OUT"; fi
ls -la "$PKG_OUT"
