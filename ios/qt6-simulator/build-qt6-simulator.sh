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
# Builds a Qt 6 for the iOS Simulator (arm64) on an Apple Silicon Mac.
#
# The Qt installer only ships x86_64 simulator slices, which cannot run on the
# arm64-only iOS 26+ simulators. This script builds qtbase, Qt Shader Tools and
# Qt Multimedia from source for iphonesimulator/arm64 into a separate prefix,
# so the installed Qt is left untouched.
#
# Usage:
#   QT_SRC=/path/to/qtbase-src ./build-qt6-simulator.sh
#
# Environment (with defaults):
#   QT_SRC          qtbase source tree            (required, must be clean)
#   QT_HOST_PATH    host Qt 6.11.1 installation   (default: $HOME/Qt/6.11.1/macos)
#   QT_SIM_PREFIX   install prefix                (default: $HOME/Qt6-sim-arm64)
#   BUILD_ROOT      build directory               (default: $HOME/Qt6-sim-build)
#   QT_VERSION_TAG  Qt version to check out       (default: v6.11.1)
#   JOBS            parallel build jobs           (default: number of CPUs)
##############################################################################

set -eu

QT_HOST_PATH=${QT_HOST_PATH:-$HOME/Qt/6.11.1/macos}
QT_SIM_PREFIX=${QT_SIM_PREFIX:-$HOME/Qt6-sim-arm64}
BUILD_ROOT=${BUILD_ROOT:-$HOME/Qt6-sim-build}
QT_VERSION_TAG=${QT_VERSION_TAG:-v6.11.1}
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

if [ -z "${QT_SRC:-}" ]; then
    echo "error: set QT_SRC to a clean qtbase source tree" >&2
    exit 1
fi

# Qt's configure needs CMake and Ninja; prefer the copies shipped with the
# Qt installer, which are known to work with it.
for tool_dir in "$HOME/Qt/Tools/CMake/CMake.app/Contents/bin" "$HOME/Qt/Tools/Ninja"; do
    [ -d "$tool_dir" ] && PATH="$tool_dir:$PATH"
done
export PATH

mkdir -p "$BUILD_ROOT" "$QT_SIM_PREFIX"

export QT_HOST_PATH
export QT_APPLE_SIMULATOR_ARCHS=arm64

echo "==> Applying simulator patches to $QT_SRC"
git -C "$QT_SRC" apply --check "$SCRIPT_DIR/qtbase-6.11.1-iphonesimulator-arm64.patch"
git -C "$QT_SRC" apply "$SCRIPT_DIR/qtbase-6.11.1-iphonesimulator-arm64.patch"

echo "==> Configuring qtbase for iphonesimulator/arm64"
mkdir -p "$BUILD_ROOT/qtbase"
cd "$BUILD_ROOT/qtbase"
"$QT_SRC/configure" \
    -platform macx-ios-clang -release -static \
    -sdk iphonesimulator \
    -nomake tests -nomake examples \
    -prefix "$QT_SIM_PREFIX"

echo "==> Building and installing qtbase"
cmake --build . --parallel "$JOBS"
cmake --install .

# Qt Multimedia depends on Qt Shader Tools; both are built against the qtbase
# that was installed above.
build_module() {
    name=$1
    repo=$2
    src="$BUILD_ROOT/src/$name"

    if [ ! -d "$src" ]; then
        echo "==> Fetching $name ($QT_VERSION_TAG)"
        git clone --depth 1 --branch "$QT_VERSION_TAG" "$repo" "$src"
    fi

    echo "==> Building $name"
    cd "$src"
    rm -rf build-ios-sim
        cmake -S . -B build-ios-sim -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$QT_SIM_PREFIX/lib/cmake/Qt6/qt.toolchain.cmake" \
            -DQT_HOST_PATH="$QT_HOST_PATH" \
            -DCMAKE_OSX_ARCHITECTURES=arm64 \
            -DCMAKE_INSTALL_PREFIX="$QT_SIM_PREFIX" \
            -DCMAKE_BUILD_TYPE=Release \
            -DQT_BUILD_TESTS=OFF
    cmake --build build-ios-sim --parallel "$JOBS"
    cmake --install build-ios-sim
}

build_module shadertools https://code.qt.io/qt/qtshadertools.git
build_module multimedia  https://code.qt.io/qt/qtmultimedia.git

echo
echo "==> Done. Verify the result is an iOS Simulator build:"
echo "    otool -l $QT_SIM_PREFIX/lib/QtCore.framework/QtCore | grep -A2 LC_BUILD_VERSION"
echo "    (expected: platform 7)"
