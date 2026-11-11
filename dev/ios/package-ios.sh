#!/bin/bash
# The iOS app: package-ios.sh <program built with TARGET=ios> <out folder> [<name>] [<version>]
# Writes <out>/<name>.ipa (signed ad hoc, no special entitlements: for a sideloading service that re-signs it) and
# <out>/<name>.tipa (the same app with the entitlements TrollStore keeps: a bigger memory limit and extended addressing).
# An .ipa/.tipa is a zip of Payload/<name>.app. No game files in either: they are the separate gamedata folder
# (dev/ios/stage_gamedata.py), copied into the app's folder in Files. Needs a Mac: sips, plutil, zip, ldid (Homebrew's).
set -euo pipefail
BIN=${1:?usage: package-ios.sh <program> <out folder> [name] [version]}; OUT=${2:?}; NAME=${3:-GE2RB}; VER=${4:-1.0}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
BUNDLE_ID=${BUNDLE_ID:-com.ge2rb.nativ}; MIN=${IOS_MIN:-16.0}
command -v ldid >/dev/null || { echo "package-ios: ldid is missing (brew install ldid)"; exit 1; }
[[ -f $BIN ]] || { echo "package-ios: no program at $BIN"; exit 1; }
rm -rf "$OUT/stage" "$OUT/$NAME.ipa" "$OUT/$NAME.tipa"; mkdir -p "$OUT"

make_app() {   # make_app <app dir> <entitlements file or ->
  local APP=$1 ENT=$2
  mkdir -p "$APP"; cp "$BIN" "$APP/$NAME"; chmod 755 "$APP/$NAME"
  for s in 120 180 152 167; do sips -z $s $s "$ROOT/assets/brand/png/icon-1024.png" --out "$APP/AppIcon-$s.png" >/dev/null; done
  cat > "$APP/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VER</string>
  <key>CFBundleVersion</key><string>$VER</string>
  <key>MinimumOSVersion</key><string>$MIN</string>
  <key>LSRequiresIPhoneOS</key><true/>
  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
  <key>UIRequiredDeviceCapabilities</key><array><string>arm64</string><string>metal</string></array>
  <key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
  <key>UISupportedInterfaceOrientations~ipad</key><array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
  <key>UIRequiresFullScreen</key><true/>
  <key>UIStatusBarHidden</key><true/>
  <key>UIViewControllerBasedStatusBarAppearance</key><false/>
  <key>UILaunchScreen</key><dict/>
  <key>UIFileSharingEnabled</key><true/>
  <key>LSSupportsOpeningDocumentsInPlace</key><true/>
  <key>UIApplicationSupportsIndirectInputEvents</key><true/>
  <key>NSCameraUsageDescription</key><string>The game asks for the camera (the Vita's camera); nothing is recorded.</string>
  <key>NSMicrophoneUsageDescription</key><string>The game asks for the microphone (the Vita's microphone); nothing is recorded.</string>
  <key>NSLocalNetworkUsageDescription</key><string>The game's local network play.</string>
  <key>CFBundleIcons</key><dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key><array><string>AppIcon-120.png</string><string>AppIcon-180.png</string></array></dict></dict>
  <key>CFBundleIcons~ipad</key><dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key><array><string>AppIcon-152.png</string><string>AppIcon-167.png</string></array></dict></dict>
</dict></plist>
PLIST
  plutil -lint "$APP/Info.plist" >/dev/null
  if [[ $ENT == - ]]; then ldid -S "$APP/$NAME"; else ldid -S"$ENT" "$APP/$NAME"; fi
}

cat > "$OUT/tipa-entitlements.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.developer.kernel.increased-memory-limit</key><true/>
  <key>com.apple.developer.kernel.extended-virtual-addressing</key><true/>
</dict></plist>
PLIST
make_app "$OUT/stage/ipa/Payload/$NAME.app" -
make_app "$OUT/stage/tipa/Payload/$NAME.app" "$OUT/tipa-entitlements.plist"
( cd "$OUT/stage/ipa" && zip -qry -X "$OUT/$NAME.ipa" Payload )
( cd "$OUT/stage/tipa" && zip -qry -X "$OUT/$NAME.tipa" Payload )
rm -rf "$OUT/stage"
echo "--- the program:"; lipo -info "$BIN" || true; (vtool -show-build "$BIN" 2>/dev/null | grep -E "platform|minos" ) || true
echo "--- the tipa's entitlements:"; unzip -p "$OUT/$NAME.tipa" "Payload/$NAME.app/$NAME" > "$OUT/.tipa-bin" && ldid -e "$OUT/.tipa-bin" | head -12; rm -f "$OUT/.tipa-bin"
ls -l "$OUT/$NAME.ipa" "$OUT/$NAME.tipa"
