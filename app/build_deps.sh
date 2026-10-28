#!/usr/bin/env bash
# Builds the static FFmpeg and curl that build.sh links into Glint.exe,
# with only what the app uses, so the exe needs no DLLs beyond Windows and the
# graphics driver. Run once from an MSYS2 MinGW 64-bit shell (docs/BUILD.md).
# Sources are pinned and checked; libraries and headers go to app/deps/prefix.
set -e
cd "$(dirname "$0")"

FFMPEG=ffmpeg-9.0.2
FFMPEG_URL=https://ffmpeg.org/releases/$FFMPEG.tar.xz
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
CURL=curl-8.23.0
CURL_URL=https://github.com/curl/curl/releases/download/curl-8_23_0/$CURL.tar.xz
CURL_SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5

DEPS="$PWD/deps"
PREFIX="$DEPS/prefix"
JOBS=$(nproc)
mkdir -p "$DEPS/src" "$DEPS/build"

fetch() {   # name url sha256
  local file="$DEPS/src/$1.tar.xz"
  [ -f "$file" ] || curl -fL "$2" -o "$file"
  echo "$3  $file" | sha256sum -c -
  rm -rf "$DEPS/build/$1"
  tar -xf "$file" -C "$DEPS/build"
}

fetch "$FFMPEG" "$FFMPEG_URL" "$FFMPEG_SHA256"
fetch "$CURL" "$CURL_URL" "$CURL_SHA256"

# FFmpeg: the H.264 decoder (D3D11VA on the GPU) for the live picture and the
# gallery, the AAC/PNG/H.264 encoders for recording, screenshots and streaming
# (AMF on AMD, Media Foundation elsewhere, x264 as the last resort), MKV/MP4/FLV
# out, PNG for screenshots and their gallery previews, and plain RTMP (RTMPS
# goes through the Windows TLS relay, tls_relay.cpp).
cd "$DEPS/build/$FFMPEG"
./configure \
  --prefix="$PREFIX" --pkg-config-flags=--static \
  --enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
  --disable-autodetect --enable-w32threads \
  --enable-gpl --enable-libx264 --enable-zlib \
  --enable-d3d11va --enable-amf --enable-mediafoundation \
  --disable-avdevice --disable-avfilter --disable-swresample \
  --disable-everything \
  --enable-decoder=h264,aac,png \
  --enable-hwaccel=h264_d3d11va,h264_d3d11va2 \
  --enable-encoder=aac,png,h264_amf,h264_mf,libx264 \
  --enable-parser=h264,aac,png \
  --enable-demuxer=matroska,mov,image2,png_pipe \
  --enable-muxer=matroska,mp4,flv \
  --enable-bsf=h264_mp4toannexb \
  --enable-protocol=file,rtmp
make -j"$JOBS"
make install

# curl: HTTP(S) only, with Windows' own TLS and certificate store (Schannel).
# Built with CMake + Ninja: its autotools build spends minutes in configure and
# libtool under MSYS2, and libtool cannot install into a path with spaces.
cmake -S "$DEPS/build/$CURL" -B "$DEPS/build/$CURL/out" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
  -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
  -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none \
  -DCURL_USE_LIBPSL=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF \
  -DUSE_NGHTTP2=OFF -DUSE_NGTCP2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
  -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_DICT=ON \
  -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_POP3=ON -DCURL_DISABLE_IMAP=ON \
  -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_FTP=ON \
  -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_IPFS=ON -DCURL_DISABLE_WEBSOCKETS=ON
cmake --build "$DEPS/build/$CURL/out" -j"$JOBS"
cmake --install "$DEPS/build/$CURL/out"

cd "$DEPS"
rm -rf build
echo "static FFmpeg and curl installed in app/deps/prefix"
