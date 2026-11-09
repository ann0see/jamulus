#!/bin/sh
##############################################################################
# Copyright (c) 2026
#
# Author(s):
#  ann0see
#  The Jamulus Development Team
#
# Licensed under AGPL 3.0 or any later version. See COPYING for details.
##############################################################################
#
# Builds Jamulus for the iOS Simulator against the Qt 6 built by
# build-qt6-simulator.sh, then installs and launches it in a booted simulator.
#
# Usage:
#   ./build-jamulus-simulator.sh [device]
#
# "device" is a simulator name or UDID ("booted" by default).
#
# Environment (with defaults):
#   QT_SIM_PREFIX       Qt for the simulator   (default: $HOME/Qt6-sim-arm64)
#   BUILD_DIR           Jamulus build dir      (default: build-ios-sim-qt6)
#   DEPLOYMENT_TARGET   iOS deployment target  (default: 17.0)
##############################################################################

set -eu

QT_SIM_PREFIX=${QT_SIM_PREFIX:-$HOME/Qt6-sim-arm64}
DEVICE=${1:-booted}
BUILD_DIR=${BUILD_DIR:-build-ios-sim-qt6}
DEPLOYMENT_TARGET=${DEPLOYMENT_TARGET:-17.0}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)

if [ ! -x "$QT_SIM_PREFIX/bin/qmake" ]; then
    echo "error: $QT_SIM_PREFIX/bin/qmake not found; build Qt 6 first" >&2
    exit 1
fi

cd "$PROJECT_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "==> Generating the Xcode project"
# add_ios_ffmpeg_libraries is only needed for Qt builds that ship FFmpeg;
# Jamulus itself only uses QSoundEffect.
"$QT_SIM_PREFIX/bin/qmake" ../Jamulus.pro -spec macx-ios-clang -config release \
    "CONFIG-=add_ios_ffmpeg_libraries" \
    "CONFIG+=sdk_no_version_check"

make -f Jamulus.xcodeproj/qt_makeqmake.mak
make -f Jamulus.xcodeproj/qt_preprocess.mak

echo "==> Building"
# -target is used because qmake-generated projects do not declare
# SUPPORTED_PLATFORMS, which -scheme would need. The deployment target has to be
# overridden because the mkspec still says 12.0, which Xcode 27 rejects.
xcodebuild -project Jamulus.xcodeproj -target Jamulus \
    -configuration Release -sdk iphonesimulator \
    ARCHS=arm64 ONLY_ACTIVE_ARCH=YES \
    IPHONEOS_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" build

APP="Release-iphonesimulator/Jamulus.app"
echo "==> Built $APP"
xcrun simctl install "$DEVICE" "$APP"
xcrun simctl terminate "$DEVICE" app.jamulussoftware.Jamulus 2> /dev/null || true
xcrun simctl launch "$DEVICE" app.jamulussoftware.Jamulus

echo
echo "Screenshot (devices with more than one display need --display=N):"
echo "    xcrun simctl io $DEVICE screenshot --display=3 --type=png /tmp/jamulus.png"
