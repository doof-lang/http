#!/bin/sh
# Builds lib/libcurl.a against the system OpenSSL and installs the public curl
# headers into include/curl/. std/http declares this build only for Linux.
# Runs inside a private copy of vendor/ with CC, AR, RANLIB, CFLAGS, and
# DOOF_JOBS supplied by the Doof compiler.
set -eu

root=$(pwd)
jobs=${DOOF_JOBS:-1}

mkdir -p work
(
  cd work
  ../curl/configure \
    --disable-shared \
    --enable-static \
    --disable-ldap \
    --disable-ldaps \
    --without-brotli \
    --without-libidn2 \
    --without-libpsl \
    --without-libssh2 \
    --without-nghttp2 \
    --without-nghttp3 \
    --without-ngtcp2 \
    --without-zlib \
    --without-zstd \
    --with-openssl \
    --disable-dependency-tracking \
    --prefix="$root" \
    --no-create
  # The curated tree keeps only the library, so generate just its makefiles
  # rather than every directory configure.ac lists.
  ./config.status lib/Makefile lib/libcurl.vers lib/curl_config.h include/Makefile include/curl/Makefile libtool
)

make -C work/lib "-j$jobs"
make -C work/include install
make -C work/lib install
