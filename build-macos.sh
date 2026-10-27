#!/bin/bash
# Builds obs-plugin-checker.plugin for macOS without Xcode or CMake (Command Line Tools are enough).
#
#   ./build-macos.sh            build into build/obs-plugin-checker.plugin
#   ./build-macos.sh install    build and copy to ~/Library/Application Support/obs-studio/plugins
#
# Headers: OBS 31.1.1 + Qt 6.8.3 (same versions as obsproject/obs-plugintemplate), so the
# plugin loads in OBS 31.1, 32.x and 33. Binaries are linked against the OBS.app that is installed.
set -euo pipefail

NAME=obs-plugin-checker
VERSION=0.1.0
BUNDLE_ID=com.n3ckarmann.obs-plugin-checker
OBS_APP=${OBS_APP:-/Applications/OBS.app}

OBS_SRC_VERSION=31.1.1
OBS_SRC_SHA256=39751f067bacc13d44b116c5138491b5f1391f91516d3d590d874edd21292291
QT_DEPS_VERSION=2025-07-11
QT_DEPS_SHA256=d3f5f04b6ea486e032530bdf0187cbda9a54e0a49621a4c8ba984c5023998867

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD=$ROOT/build
DEPS=${OBS_DEPS_DIR:-$BUILD/deps}
OBS_FW=$OBS_APP/Contents/Frameworks

fetch() { # url sha256 target
	if [ ! -f "$3" ]; then
		echo "Lade $1"
		curl -fL --progress-bar -o "$3.part" "$1"
		mv "$3.part" "$3"
	fi
	echo "$2  $3" | shasum -a 256 -c - >/dev/null || { echo "Prüfsumme falsch: $3"; rm -f "$3"; exit 1; }
}

mkdir -p "$DEPS"
if [ ! -f "$DEPS/obs/libobs/obs.h" ]; then
	fetch "https://github.com/obsproject/obs-studio/archive/refs/tags/$OBS_SRC_VERSION.tar.gz" \
		"$OBS_SRC_SHA256" "$DEPS/obs-$OBS_SRC_VERSION.tar.gz"
	mkdir -p "$DEPS/obs"
	tar -xzf "$DEPS/obs-$OBS_SRC_VERSION.tar.gz" -C "$DEPS/obs" --strip-components=1
fi
if [ ! -d "$DEPS/qt6/lib/QtCore.framework/Headers" ]; then
	fetch "https://github.com/obsproject/obs-deps/releases/download/$QT_DEPS_VERSION/macos-deps-qt6-$QT_DEPS_VERSION-universal.tar.xz" \
		"$QT_DEPS_SHA256" "$DEPS/qt6-$QT_DEPS_VERSION.tar.xz"
	mkdir -p "$DEPS/qt6"
	tar -xJf "$DEPS/qt6-$QT_DEPS_VERSION.tar.xz" -C "$DEPS/qt6"
fi

[ -d "$OBS_FW/libobs.framework" ] || { echo "OBS nicht gefunden unter $OBS_APP (OBS_APP setzen)"; exit 1; }
ARCHS=$(lipo -archs "$OBS_FW/libobs.framework/libobs")

GEN=$BUILD/generated
mkdir -p "$GEN"
cat >"$GEN/obsconfig.h" <<'EOF'
#pragma once
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
EOF
# Qt 6.8 headers call __yield() without declaring it; newer Apple clang treats that as an error.
cat >"$GEN/qt-compat.h" <<'EOF'
#if defined(__aarch64__)
#include <arm_acle.h>
#endif
EOF

QT=$DEPS/qt6/lib
CXXFLAGS=(-std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wno-deprecated-declarations
	-mmacosx-version-min=12.0
	-DPLUGIN_VERSION="\"$VERSION\""
	-include "$GEN/qt-compat.h" -I"$GEN" -I"$DEPS/obs/libobs" -I"$DEPS/obs/frontend/api"
	-F"$QT" -I"$QT/QtCore.framework/Headers" -I"$QT/QtGui.framework/Headers"
	-I"$QT/QtWidgets.framework/Headers")
for a in $ARCHS; do CXXFLAGS+=(-arch "$a"); done

OBJ=$BUILD/obj
mkdir -p "$OBJ"
for src in "$ROOT"/src/*.cpp; do
	echo "Kompiliere $(basename "$src")"
	clang++ "${CXXFLAGS[@]}" -c "$src" -o "$OBJ/$(basename "${src%.cpp}").o"
done

PLUGIN=$BUILD/$NAME.plugin
rm -rf "$PLUGIN"
mkdir -p "$PLUGIN/Contents/MacOS" "$PLUGIN/Contents/Resources"

echo "Linke $NAME ($ARCHS)"
LDARCH=()
for a in $ARCHS; do LDARCH+=(-arch "$a"); done
clang++ "${LDARCH[@]}" -bundle -mmacosx-version-min=12.0 -o "$PLUGIN/Contents/MacOS/$NAME" "$OBJ"/*.o \
	-F"$OBS_FW" -framework libobs -framework QtCore -framework QtGui -framework QtWidgets \
	"$OBS_FW/obs-frontend-api.dylib" -framework CoreFoundation -lcurl \
	-Wl,-rpath,@executable_path/../Frameworks

cp -R "$ROOT/data/locale" "$PLUGIN/Contents/Resources/"
cp "$ROOT/data/known-plugins.json" "$PLUGIN/Contents/Resources/"
sed -e "s/@VERSION@/$VERSION/" -e "s/@OS_ARCH@/macos-$(echo $ARCHS | tr ' ' '-')/" \
	"$ROOT/cmake/manifest.json.in" >"$PLUGIN/Contents/Resources/manifest.json"

cat >"$PLUGIN/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>$NAME</string>
	<key>CFBundleDisplayName</key><string>Plugin Checker</string>
	<key>CFBundleExecutable</key><string>$NAME</string>
	<key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
</dict>
</plist>
EOF

codesign --force --sign - "$PLUGIN"
echo "Fertig: $PLUGIN"

if [ "${1:-}" = "install" ]; then
	DEST="$HOME/Library/Application Support/obs-studio/plugins"
	mkdir -p "$DEST"
	rm -rf "$DEST/$NAME.plugin"
	cp -R "$PLUGIN" "$DEST/"
	echo "Installiert nach $DEST/$NAME.plugin (OBS neu starten)"
fi

if [ "${1:-}" = "test" ]; then
	# Headless check against the installed OBS: load test, plugin scan, GitHub query, dialog screenshot.
	echo "Baue Test-Harness"
	clang++ "${CXXFLAGS[@]}" "$ROOT/tools/harness.cpp" "$ROOT"/src/*.cpp -o "$BUILD/harness" \
		-F"$OBS_FW" -framework libobs -framework QtCore -framework QtGui -framework QtWidgets \
		"$OBS_FW/obs-frontend-api.dylib" -framework CoreFoundation -lcurl -Wl,-rpath,"$OBS_FW"
	mkdir -p "$BUILD/test-config"
	HARNESS_CONFIG="$BUILD/test-config" QT_QPA_PLATFORM=offscreen "$BUILD/harness" "$PLUGIN" "${2:-cache}" "$BUILD/dialog.png"
fi
