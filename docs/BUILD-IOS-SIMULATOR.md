# Building Jamulus for the iOS Simulator (Qt 6, Apple Silicon)

Instructions to build and run Jamulus in an iOS Simulator on an Apple Silicon
Mac with Xcode 27 and iOS 27 simulators.

This is a record of a local validation, not a supported build recipe. Expect it
to change as the Qt 6 migration progresses (see
[issue #3406](https://github.com/jamulussoftware/jamulus/issues/3406)).

The whole process is automated by two scripts in `ios/qt6-simulator/`:

1. `build-qt6-simulator.sh` — builds Qt 6 (qtbase, Qt Shader Tools, Qt
   Multimedia) for `iphonesimulator`/`arm64` into a private prefix.
2. `build-jamulus-simulator.sh` — builds Jamulus against that prefix and
   installs and launches it in a simulator.

```sh
QT_SRC=~/Proj/qtbase-6.11.1-sim \
QT_HOST_PATH=~/Qt/6.11.1/macos \
    ./ios/qt6-simulator/build-qt6-simulator.sh

./ios/qt6-simulator/build-jamulus-simulator.sh "iPhone Duo"
```

The sections below explain why this is necessary and what the scripts do.

## Why a source build is needed

The Qt kits installed by the Qt installer contain a device slice and a
simulator slice, but the simulator slice is x86_64:

```
$ lipo -info ~/Qt/6.11.1/ios/lib/QtCore.framework/QtCore
Architectures in the fat file: ... are: x86_64 arm64

$ lipo ~/Qt/6.11.1/ios/lib/QtCore.framework/QtCore -thin arm64 -output /tmp/c && \
  otool -l /tmp/c | grep -A2 LC_BUILD_VERSION
 platform 2        # iOS device

$ lipo ~/Qt/6.11.1/ios/lib/QtCore.framework/QtCore -thin x86_64 -output /tmp/x && \
  otool -l /tmp/x | grep -A2 LC_BUILD_VERSION
 platform 7        # iOS Simulator, x86_64 only
```

Current simulator runtimes are arm64-only, so the prebuilt simulator slice
cannot be used at all. Qt has to be built from source for
`iphonesimulator`/`arm64`.

## Why Qt 6 and not Qt 5

Qt 5.15 renders into a `CAEAGLLayer` (OpenGL ES). On iOS 26/27 simulators that
content is never composited: the framebuffer contains the correct pixels
(verified with `glReadPixels` immediately before presenting) and
`presentRenderbuffer` returns `YES` with `glGetError() == 0`, but the display
stays black. The view is in the window hierarchy and both the layer bounds and
`contentsScale` are correct.

A 20 line Qt Widgets application built against the same Qt 5 shows the same
black screen, so this is a Qt 5 / simulator limitation, not a Jamulus bug.

Qt 6 uses QRhi with a Metal swapchain and renders correctly.

## Qt-side patches

`ios/qt6-simulator/qtbase-6.11.1-iphonesimulator-arm64.patch` applies to
qtbase 6.11.1 and contains three changes:

1. **`cmake/QtAutoDetectHelpers.cmake`** — Qt hardcodes `x86_64` for
   `QT_APPLE_SDK=iphonesimulator`. The patch allows overriding it through the
   `QT_APPLE_SIMULATOR_ARCHS` environment variable.

2. **`src/plugins/platforms/ios/qiosscreen.mm`** (`QIOSScreen::initializeScreens`)
   — the first screen added becomes Qt's primary screen. `UIScreen.mainScreen`
   is not necessarily the display the application is shown on (on foldables the
   app is hosted in a `UIWindowScene` on a different display), and the order of
   `UIScreen.screens` is not stable between launches. The patch prefers the
   screen of a connected window scene and otherwise the largest screen.

3. **`src/plugins/platforms/ios/qiosglobal.mm`** (`rootViewForScreen`) — when no
   window scene matches the requested screen, fall back to the application's own
   scene instead of returning `nullptr`. Without this, Qt adds its views to
   nothing and renders into a display that is not visible.

With (2) and (3) Jamulus renders reliably on the iPhone Duo; without them the
app renders on roughly one launch in three.

## Jamulus-side changes required by Qt 6

* **`Jamulus.pro`**: `src/ios/ios_app_delegate.mm` defines a second
  `QIOSApplicationDelegate`. Qt 6's own implementation provides the UIScene
  lifecycle adoption required since iOS 26; a second implementation shadows
  those methods and the app aborts on launch with
  *"Application failed to launch: UIScene life cycle is required for apps built
  with this SDK"* (`SIGTRAP`). The file is now only compiled for Qt 5.

The large `CClient`/`CServer` objects are additionally allocated on the heap,
because iOS gives the main thread a 1 MB stack and `main()` reserves roughly
341 KB for them. That is a separate change and is not part of this one; see
[issue #3808](https://github.com/jamulussoftware/jamulus/issues/3808).

## What the scripts do

### Qt

```sh
git -C <qtbase-src> apply ios/qt6-simulator/qtbase-6.11.1-iphonesimulator-arm64.patch

QT_HOST_PATH=~/Qt/6.11.1/macos QT_APPLE_SIMULATOR_ARCHS=arm64 \
  <qtbase-src>/configure -platform macx-ios-clang -release -static \
      -sdk iphonesimulator -nomake tests -nomake examples \
      -prefix ~/Qt6-sim-arm64
cmake --build . --parallel
cmake --install .
```

`QT_HOST_PATH` must point at a host Qt 6.11.1 installation; the prebuilt macOS
kit works. The source tree must be clean (an in-source build prevents shadow
builds), which is why the script expects a separate checkout or git worktree:

```sh
git -C ~/Proj/Qt6 worktree add ~/Proj/qtbase-6.11.1-sim v6.11.1
```

Verify the result:

```
$ otool -l ~/Qt6-sim-arm64/lib/QtCore.framework/QtCore | grep -A2 LC_BUILD_VERSION
 platform 7
```

Qt Multimedia needs Qt Shader Tools in the same prefix; both are built with the
same toolchain file and installed into `~/Qt6-sim-arm64`.

qtmultimedia is built without FFmpeg. Jamulus only uses `QSoundEffect`, so the
project's iOS FFmpeg configuration (`CONFIG+=add_ios_ffmpeg_libraries`) is
disabled for such a build.

### Jamulus

```sh
mkdir -p build-ios-sim-qt6 && cd build-ios-sim-qt6
~/Qt6-sim-arm64/bin/qmake ../Jamulus.pro -spec macx-ios-clang -config release \
    "CONFIG-=add_ios_ffmpeg_libraries" "CONFIG+=sdk_no_version_check"

make -f Jamulus.xcodeproj/qt_makeqmake.mak
make -f Jamulus.xcodeproj/qt_preprocess.mak

xcodebuild -project Jamulus.xcodeproj -target Jamulus \
    -configuration Release -sdk iphonesimulator \
    ARCHS=arm64 ONLY_ACTIVE_ARCH=YES IPHONEOS_DEPLOYMENT_TARGET=17.0 build
```

`-target` is used instead of `-scheme` because qmake-generated projects do not
declare `SUPPORTED_PLATFORMS`. `IPHONEOS_DEPLOYMENT_TARGET` must be overridden
because the mkspec still says 12.0, while Xcode 27 only accepts 15.0 and later.

## Running

```sh
xcrun simctl boot "iPhone Duo"
xcrun simctl install booted Release-iphonesimulator/Jamulus.app
xcrun simctl launch booted app.jamulussoftware.Jamulus
xcrun simctl io booted screenshot --display=3 --type=png /tmp/jamulus.png
```

Devices with more than one display (iPhone Duo) need an explicit `--display=`.
`xcrun simctl io booted screenshot` without it fails with `Invalid argument`.

## Findings

* Jamulus starts, stays alive and renders its main window on both a regular
  iPhone simulator and on the iPhone Duo, when built with Qt 6 as described
  above. The UI is complete: menu bar, input level meters, reverb slider,
  Ping/Delay/Jitter, the mixer board area and the Connect button.
* The Qt 5 crash and black screen reported in
  [issue #3808](https://github.com/jamulussoftware/jamulus/issues/3808) are
  covered by the changes listed above. On the Duo the remaining black screen
  was caused by Qt choosing a display that is not the visible one; that is what
  patches (2) and (3) address.
* Interactive behaviour (connect dialog, server connection, chat window,
  language change) has **not** been validated yet. The problems listed in
  [issue #3406](https://github.com/jamulussoftware/jamulus/issues/3406) —
  chat window proportions, hangs when closing the chat window, language change
  — should be expected to still be present until tested.

## Known issues

* Qt 6 on iOS still has UI problems unrelated to the build itself, see
  [issue #3406](https://github.com/jamulussoftware/jamulus/issues/3406).
* Qt 5 cannot render in iOS 26/27 simulators at all, and Qt 5 has no UIScene
  support, so a Qt 5 build is not usable with current SDKs.
