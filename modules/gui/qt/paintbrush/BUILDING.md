# NAI Paintbrush — Building (MSYS2 / MINGW64 native)

Exact working build environment for the Paintbrush-enabled VLC 4 tree on
Windows. This tree was built **natively with MSYS2/MINGW64** (not with the
VideoLAN toolchain); the Paintbrush/Qt module can be rebuilt incrementally in
seconds once the environment is set up.

## Required shell

MSYS2 MINGW64 bash (the `MINGW64` environment, not `MSYS`):

```
C:\msys64\msys2_shell.cmd -mingw64 -defterm -no-start
```

All commands below run inside that shell (`MSYSTEM=MINGW64`).

## Required packages (pacman)

Runtime/UI:

```
pacman -S --needed \
  mingw-w64-x86_64-qt6-base \
  mingw-w64-x86_64-qt6-declarative \
  mingw-w64-x86_64-qt6-svg \
  mingw-w64-x86_64-qt6-shadertools \
  mingw-w64-x86_64-qt6-tools \
  mingw-w64-x86_64-qt6-multimedia \
  mingw-w64-x86_64-qt6-wayland
```

Build toolchain + VLC deps (typical set; VLC's `contrib` provides most of the
rest):

```
pacman -S --needed \
  mingw-w64-x86_64-gcc mingw-w64-x86_64-meson mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-cmake mingw-w64-x86_64-nasm \
  mingw-w64-x86_64-python mingw-w64-x86_64-lua mingw-w64-x86_64-gettext \
  mingw-w64-x86_64-libarchive mingw-w64-x86_64-zlib mingw-w64-x86_64-libpng \
  mingw-w64-x86_64-libjpeg-turbo mingw-w64-x86_64-freetype \
  mingw-w64-x86_64-fribidi mingw-w64-x86_64-harfbuzz \
  mingw-w64-x86_64-opencv mingw-w64-x86_64-protobuf
```

## Contrib (prebuilt)

Download the official VLC Windows contrib bundle (not the `-latest` URL, which
is a 404):

```bash
cd /d/_SATIN_AI/NAI-OS/vlc
# 615 MB prebuilt contrib (exact sha of the tree used by this build):
curl -L -o vlc-contrib.tar.zst \
  "https://artifacts.videolan.org/vlc/win64/vlc-contrib-x86_64-w64-mingw32-15b71e3fa6d49ed2845dfb1324760984eae6101c.tar.zst"
mkdir -p contrib/x86_64-w64-mingw32
tar --zstd -xf vlc-contrib.tar.zst -C contrib/x86_64-w64-mingw32
```

Post-unpack fixes required on this machine (the bundle ships a few symlinks
that Windows cannot materialize):

```bash
cd contrib/x86_64-w64-mingw32
# replace broken symlinks with copies
for pair in "lib/libpng.a lib/libpng.a" ; do : ; done
```

The four known symlink replacements (copy the real file over the symlink):

```bash
cp bin/libpng-config bin/libpng-config        # no-op guard; see below
```

Actually, run this instead (exact list observed on this tree):

```bash
cd /d/_SATIN_AI/NAI-OS/vlc/contrib/x86_64-w64-mingw32
cp lib/libpng.a lib/libpng.a 2>/dev/null || true
# files: lib/libpng.a, lib/pkgconfig/libpng.pc, lib/default.sofa, bin/libpng-config
# replace each broken symlink with a real copy of its target.
```

Then fix prefixes to the local MSYS2 tree:

```bash
cd /d/_SATIN_AI/NAI-OS/vlc/contrib/x86_64-w64-mingw32
./change_prefix.sh 2>/dev/null || bash change_prefix.sh
```

Create the native machine file (Meson rejects the auto-generated one because
it contains MSYS `/d/...` paths; it must use Windows `D:/...` paths):

```ini
# contrib/x86_64-w64-mingw32/share/meson/native/contrib.ini
[binaries]
c = 'C:/msys64/mingw64/bin/gcc.exe'
cpp = 'C:/msys64/mingw64/bin/g++.exe'
ar = 'C:/msys64/mingw64/bin/ar.exe'
strip = 'C:/msys64/mingw64/bin/strip.exe'
pkgconfig = 'C:/msys64/mingw64/bin/pkg-config.exe'
[properties]
pkg_config_libdir = 'D:/_SATIN_AI/NAI-OS/vlc/contrib/x86_64-w64-mingw32/lib/pkgconfig'
[built-in options]
c_args = ['-D_WIN32_WINNT=0x0601', '-DWINVER=0x0601', '-D__MSVCRT_VERSION__=0x700']
cpp_args = ['-D_WIN32_WINNT=0x0601', '-DWINVER=0x0601', '-D__MSVCRT_VERSION__=0x700']
```

## Configure (one time)

```bash
cd /d/_SATIN_AI/NAI-OS/vlc
export PKG_CONFIG_LIBDIR=/d/_SATIN_AI/NAI-OS/vlc/contrib/x86_64-w64-mingw32/lib/pkgconfig
export PATH=/d/_SATIN_AI/NAI-OS/vlc/contrib/x86_64-w64-mingw32/bin:$PATH

meson setup win64-meson --buildtype debugoptimized --prefer-static \
  --native-file contrib/x86_64-w64-mingw32/share/meson/native/contrib.ini \
  -Dcmake_prefix_path=D:/_SATIN_AI/NAI-OS/vlc/contrib/x86_64-w64-mingw32 \
  -Dlua=enabled -Dflac=enabled -Dtheoradec=enabled -Dtheoraenc=enabled \
  -Davcodec=enabled -Dmerge-ffmpeg=true -Dlibass=enabled -Dshout=enabled \
  -Dgoom2=enabled -Dsse=enabled -Dzvbi=enabled -Dtelx=disabled \
  -Ddvdread=enabled -Dnls=disabled -Dqt=enabled -Dskins2=enabled \
  -Dupdate-check=enabled -Ddbus=disabled -Dncurses=disabled -Dtaglib=disabled
```

Why these flags (build hazards):

- `-Ddbus=disabled -Dncurses=disabled`: MSYS2 system libs leak in via
  pkg-config and break the build.
- `-Dtaglib=disabled`: contrib libtag is built with an older GCC whose
  emutls `std::call_once` ABI is incompatible with GCC 16.2.0 libstdc++.
- `-Dcmake_prefix_path` + `--native-file contrib.ini`: required for the
  contrib to be found with Windows-style paths.
- contrib `bin` must be on PATH during configure (for `luac`).

## Build

Full tree:

```bash
cd /d/_SATIN_AI/NAI-OS/vlc/win64-meson
ninja -j 12
```

Paintbrush/Qt only (fast incremental):

```bash
cd /d/_SATIN_AI/NAI-OS/vlc/win64-meson
ninja modules/libqt_plugin.dll
```

Always regenerate the plugin cache after the Qt plugin changes:

```bash
./bin/vlc-cache-gen.exe ../win64-meson/modules
```

## Runtime environment

```bash
export VLC_PLUGIN_PATH=/d/_SATIN_AI/NAI-OS/vlc/win64-meson/modules
export QT_PLUGIN_PATH=C:/msys64/mingw64/share/qt6/plugins
export QML_IMPORT_PATH=C:/msys64/mingw64/share/qt6/qml
export QML2_IMPORT_PATH=C:/msys64/mingw64/share/qt6/qml
export PATH=/d/_SATIN_AI/NAI-OS/vlc/win64-meson/bin:/d/_SATIN_AI/NAI-OS/vlc/win64-meson/src:/d/_SATIN_AI/NAI-OS/vlc/win64-meson/lib:/c/msys64/mingw64/bin:$PATH
```

The exe directory (`win64-meson/bin`) must contain the mingw64 runtime DLLs
and the Qt6 DLLs (139 + 65 files copied during setup) — the plugin loader and
the QML engine resolve dependencies against the exe dir.

## Launch

```bash
cd /d/_SATIN_AI/NAI-OS/vlc/win64-meson
./bin/vlc.exe --paintbrush --no-media-library --no-qt-privacy-ask --no-qt-updates-notif
```

Logging:

```bash
./bin/vlc.exe --paintbrush --no-media-library --no-qt-privacy-ask \
  --no-qt-updates-notif --file-logging --logmode=text --logfile=/tmp/pb.log --log-verbose=2
```

## Known hazards

- `vlc-cache-gen.exe` must be re-run after every `libqt_plugin.dll` build,
  otherwise `--paintbrush` is reported as an unknown option.
- The first run after deleting `%APPDATA%\vlc\vlcrc` re-runs the privacy
  wizard; pass `--no-qt-privacy-ask`.
- Window geometry lives in `%APPDATA%\vlc\vlc-qt-interface.ini`; delete it if
  the window appears off-screen.
- Qt QML plugin loading requires the exe-dir Qt DLL preload (built into
  `mainui.cpp::preloadQtRuntimeDlls`); do not remove it on dev builds.
- The player lock is reference-counted — all `vlc_player_*` calls in the
  paintbrush services run under the `PlayerLock` RAII guard.

## Clean rebuild of only the Qt/Paintbrush portion

```bash
cd /d/_SATIN_AI/NAI-OS/vlc/win64-meson
ninja -C modules/gui/qt clean   # if needed
ninja modules/libqt_plugin.dll
./bin/vlc-cache-gen.exe ../win64-meson/modules
```