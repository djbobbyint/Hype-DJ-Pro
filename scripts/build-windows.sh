#!/usr/bin/env bash
# Cross-builds HYPE DJ PRO for 64-bit Windows on Ubuntu 24.04 and packages it.
#
#   scripts/build-windows.sh [deps-dir] [build-dir]
#
# Toolchain: the distribution's Clang 18 targeting x86_64-w64-windows-gnu with the
# distribution's mingw-w64 sysroot and libstdc++. JUCE 8 officially targets MSVC, so a few
# headers newer than Ubuntu's mingw-w64 v11 are taken from upstream mingw-w64 (see step 2).
# On Windows itself you can instead build natively with Visual Studio 2022 (docs/BUILDING.md).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${1:-$ROOT/../hype-dj-pro-deps}"
BUILD="${2:-$ROOT/build-win}"
mkdir -p "$DEPS"
DEPS="$(cd "$DEPS" && pwd)"

echo "== 0. Tools"
need=(clang lld g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix mingw-w64-tools cmake ninja-build git make nsis)
missing=()
for p in "${need[@]}"; do dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p"); done
if [ ${#missing[@]} -gt 0 ]; then
    echo "Installing: ${missing[*]}"; apt-get install -y "${missing[@]}"
fi

echo "== 1. FFmpeg (LGPL, audio decoders only, shared DLLs, no external runtime DLLs)"
if [ ! -f "$DEPS/ffmpeg-win64/bin/avcodec-60.dll" ]; then
    [ -d "$DEPS/ffmpeg-src" ] || git clone -q --depth 1 --branch n6.1.1 https://github.com/FFmpeg/FFmpeg.git "$DEPS/ffmpeg-src"
    rm -rf "$DEPS/ffmpeg-build" && mkdir -p "$DEPS/ffmpeg-build" && cd "$DEPS/ffmpeg-build"
    "$DEPS/ffmpeg-src/configure" --prefix="$DEPS/ffmpeg-win64" --enable-cross-compile \
        --cross-prefix=x86_64-w64-mingw32- --target-os=mingw32 --arch=x86_64 \
        --cc=x86_64-w64-mingw32-gcc-posix --cxx=x86_64-w64-mingw32-g++-posix \
        --enable-shared --disable-static --disable-programs --disable-doc --disable-x86asm \
        --disable-network --disable-autodetect --enable-w32threads --disable-pthreads \
        --extra-ldflags="-static-libgcc -static" \
        --disable-everything --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
        --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-protocol=file \
        --enable-decoder=aac,aac_latm,aac_fixed,alac,mp3,mp3float,mp2,vorbis,opus,flac,wavpack,pcm_s16le,pcm_s24le,pcm_f32le \
        --enable-demuxer=mov,aac,mp3,ogg,matroska,flac,wav,aiff,wv,asf \
        --enable-parser=aac,aac_latm,mpegaudio,vorbis,opus,flac --enable-bsf=aac_adtstoasc > configure.log
    make -j"$(nproc)" > make.log && make install > install.log
fi

echo "== 2. mingw-w64 compatibility headers for JUCE 8"
COMPAT="$DEPS/mingw-compat"
if [ ! -f "$COMPAT/.complete" ]; then
    [ -d "$DEPS/mingw-w64-src" ]    || git clone -q --depth 1 --branch v12.0.0 https://github.com/mingw-w64/mingw-w64.git "$DEPS/mingw-w64-src"
    [ -d "$DEPS/mingw-w64-master" ] || git clone -q --depth 1 https://github.com/mingw-w64/mingw-w64.git "$DEPS/mingw-w64-master"
    rm -rf "$COMPAT" && mkdir -p "$COMPAT"
    H12="$DEPS/mingw-w64-src/mingw-w64-headers/include"
    HM="$DEPS/mingw-w64-master/mingw-w64-headers/include"
    cp "$H12"/d2d1*.h "$H12"/dcommon.h "$H12"/dwrite*.h "$H12"/d2dbasetypes.h "$COMPAT"/   # Direct2D 1.3 (JUCE 8 renderer)
    cp "$HM"/uiautomation.h "$HM"/uiautomationclient.h "$HM"/uiautomationcore.h "$HM"/uiautomationcoreapi.h "$COMPAT"/
    echo '#include <dbghelp.h>' > "$COMPAT/Dbghelp.h"                                      # case-sensitive file systems
    python3 - "$COMPAT" <<'PY'
import sys
c = sys.argv[1] + '/'
s = open(c + 'dcommon.h').read()
s = s.replace("} DWRITE_GLYPH_IMAGE_FORMATS;\n", """} DWRITE_GLYPH_IMAGE_FORMATS;
#ifdef __cplusplus
/* Flag operators as declared by the Windows SDK (constexpr, unlike older mingw-w64 macros). */
extern "C++" {
constexpr DWRITE_GLYPH_IMAGE_FORMATS operator| (DWRITE_GLYPH_IMAGE_FORMATS a, DWRITE_GLYPH_IMAGE_FORMATS b) { return (DWRITE_GLYPH_IMAGE_FORMATS) ((int) a | (int) b); }
constexpr DWRITE_GLYPH_IMAGE_FORMATS operator& (DWRITE_GLYPH_IMAGE_FORMATS a, DWRITE_GLYPH_IMAGE_FORMATS b) { return (DWRITE_GLYPH_IMAGE_FORMATS) ((int) a & (int) b); }
constexpr DWRITE_GLYPH_IMAGE_FORMATS operator~ (DWRITE_GLYPH_IMAGE_FORMATS a) { return (DWRITE_GLYPH_IMAGE_FORMATS) ~(int) a; }
inline DWRITE_GLYPH_IMAGE_FORMATS& operator|= (DWRITE_GLYPH_IMAGE_FORMATS& a, DWRITE_GLYPH_IMAGE_FORMATS b) { return a = a | b; }
inline DWRITE_GLYPH_IMAGE_FORMATS& operator&= (DWRITE_GLYPH_IMAGE_FORMATS& a, DWRITE_GLYPH_IMAGE_FORMATS b) { return a = a & b; }
}
#endif
""", 1)
open(c + 'dcommon.h', 'w').write(s)
s = open(c + 'uiautomationcore.h').read()
marker = "#endif /* __uiautomationcore_h__ */"
s = s.replace(marker, """/* Not yet in mingw-w64: from the Windows SDK's UIAutomationCore.h */
#ifndef HYPE_COMPAT_CARETPOSITION
#define HYPE_COMPAT_CARETPOSITION
enum CaretPosition { CaretPosition_Unknown = 0, CaretPosition_EndOfLine = 1, CaretPosition_BeginningOfLine = 2 };
#endif
""" + marker)
open(c + 'uiautomationcore.h', 'w').write(s)
PY
    touch "$COMPAT/.complete"
fi

echo "== 3. Configure and build"
cd "$ROOT"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/clang-mingw-w64-x86_64.cmake \
      -DHYPE_FFMPEG_ROOT="$DEPS/ffmpeg-win64" -DHYPE_MINGW_COMPAT_INCLUDE="$COMPAT"
cmake --build "$BUILD" -j"$(nproc)"

echo "== 4. Package"
PKG="$BUILD/package/HYPE-DJ-PRO-windows-x64"
rm -rf "$BUILD/package" && mkdir -p "$PKG/tests"
cp "$BUILD/HypeDJPro_artefacts/Release/HYPE DJ PRO.exe" "$BUILD"/HypeDJPro_artefacts/Release/*.dll "$PKG/"
cp "$BUILD/hype_cli.exe" "$PKG/"
cp "$BUILD/hype_engine_tests.exe" "$BUILD/hype_phase1_tests.exe" "$BUILD"/*.dll "$PKG/tests/"
cp "$ROOT/packaging/windows/README-WINDOWS.txt" "$PKG/README.txt"
cp "$ROOT/docs/TESTING.md" "$PKG/TESTING.md"
mkdir -p "$PKG/licenses"
cp "$DEPS/ffmpeg-src/COPYING.LGPLv2.1" "$PKG/licenses/FFmpeg-LGPL-2.1.txt"
cp "$ROOT/packaging/windows/THIRD-PARTY-NOTICES.txt" "$PKG/licenses/"
cp "$ROOT/external/JUCE/LICENSE.md" "$PKG/licenses/JUCE-LICENSE.md" 2>/dev/null || true
(cd "$BUILD/package" && rm -f ../HYPE-DJ-PRO-windows-x64.zip && python3 -c "import shutil; shutil.make_archive('../HYPE-DJ-PRO-windows-x64', 'zip', '.', 'HYPE-DJ-PRO-windows-x64')")
echo "== 5. Installer"
VERSION="$(sed -n 's/^project(HypeDJPro VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
makensis -V2 -DSRC="$PKG" -DVERSION="$VERSION" -DOUTFILE="$BUILD/HYPE-DJ-PRO-$VERSION-Setup.exe" "$ROOT/packaging/windows/installer.nsi"

echo "Done: $BUILD/HYPE-DJ-PRO-windows-x64.zip"
echo "      $BUILD/HYPE-DJ-PRO-$VERSION-Setup.exe"
