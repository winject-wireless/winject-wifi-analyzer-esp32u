#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"${CXX:-c++}" -O3 -std=c++17 -I native native/dsss_decoder.cpp tests/test_dsss_native.cpp -o "$tmp/neon"
"${CXX:-c++}" -O3 -std=c++17 -DWINJECT_DISABLE_NEON -I native native/dsss_decoder.cpp tests/test_dsss_native.cpp -o "$tmp/scalar"
"$tmp/neon" > "$tmp/neon.out"
"$tmp/scalar" > "$tmp/scalar.out"
cmp "$tmp/neon.out" "$tmp/scalar.out"
tail -n 1 "$tmp/neon.out"
printf '%s\n' 'SIMD and scalar frame outputs match'
