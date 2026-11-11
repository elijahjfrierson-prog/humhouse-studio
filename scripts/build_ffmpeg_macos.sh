#!/usr/bin/env bash
# LGPL FFmpeg (no GPL/external libs, VideoToolbox) for the VIDEO tab, universal arm64 + x86_64, macOS 11+, into $1.
set -euo pipefail
PREFIX="${1:?prefix}"; VER="${FFMPEG_VERSION:-7.1}"
[[ -f "$PREFIX/lib/libavcodec.dylib" ]] && { echo "cached FFmpeg in $PREFIX"; exit 0; }
WORK="$(mktemp -d)"; cd "$WORK"
curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz" -o ff.tar.xz && tar xf ff.tar.xz
for arch in arm64 x86_64; do
  rm -rf "b-$arch" && cp -R "ffmpeg-$VER" "b-$arch" && cd "b-$arch"
  extra=(); [[ "$arch" == x86_64 ]] && extra=(--enable-cross-compile --arch=x86_64 --target-os=darwin --disable-x86asm)
  ./configure --prefix="$WORK/p-$arch" --enable-shared --disable-static --disable-programs --disable-doc \
    --disable-autodetect --enable-videotoolbox --enable-audiotoolbox --enable-zlib --enable-pthreads \
    --disable-avdevice --disable-avfilter --disable-postproc --disable-network --install-name-dir='@rpath' \
    --cc="clang -arch $arch" --extra-cflags="-arch $arch -mmacosx-version-min=11.0" --extra-ldflags="-arch $arch -mmacosx-version-min=11.0" ${extra[@]+"${extra[@]}"}
  make -j"$(sysctl -n hw.ncpu)" >/dev/null && make install >/dev/null
  cd "$WORK"
done
mkdir -p "$PREFIX/lib" && cp -R "p-arm64/include" "$PREFIX/" && cp -R "p-arm64/lib/pkgconfig" "$PREFIX/lib/"
sed -i '' "s#$WORK/p-arm64#$PREFIX#g" "$PREFIX"/lib/pkgconfig/*.pc
for f in p-arm64/lib/*.dylib; do
  n=$(basename "$f")
  if [[ -L "$f" ]]; then cp -P "$f" "$PREFIX/lib/$n"; else lipo -create "$f" "p-x86_64/lib/$n" -output "$PREFIX/lib/$n"; fi
done
ls -la "$PREFIX/lib"; lipo -info "$PREFIX"/lib/libavcodec.*.*.dylib | head -1
