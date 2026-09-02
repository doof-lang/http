#!/bin/sh
set -eu

destination=$1
native_target=$2
configure_host=$3
target_triple=$4
sdk_path=$5
jobs=$6

case "$native_target" in
  linux)
    ;;
  *)
    echo "Skipping curl build for $native_target; std/http uses curl only on Linux."
    exit 0
    ;;
esac

cd "$destination"

prefix="$destination/.doof-build/$native_target"
stamp="$prefix/.doof-prepared"
configuration="$native_target|$configure_host|$target_triple|$sdk_path"
if [ -f "$stamp" ] && [ -f "$prefix/lib/libcurl.a" ] && [ -d "$prefix/include/curl" ] &&
  [ "$(cat "$stamp")" = "$configuration" ]; then
  exit 0
fi

if [ -f Makefile ]; then
  make distclean
fi

work_dir=".doof-build-work/$native_target"

mkdir -p "$work_dir"

common_args="
--disable-shared
--enable-static
--disable-ldap
--disable-ldaps
--without-brotli
--without-libidn2
--without-libpsl
--without-libssh2
--without-nghttp2
--without-nghttp3
--without-ngtcp2
--without-zlib
--without-zstd
"

(
  cd "$work_dir"
  ../../configure \
    $common_args \
    --with-openssl \
    --prefix="$prefix"
)

make -C "$work_dir/lib" "-j$jobs"
make -C "$work_dir/include" install
make -C "$work_dir/lib" install
printf '%s' "$configuration" > "$stamp"
