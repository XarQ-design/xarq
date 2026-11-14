#!/bin/bash
# Build libmobi into <dir>, for Mobipocket and Kindle e-books on Windows: MSYS2 doesn't package it.
#
#   build-libmobi.sh <dir>
set -euo pipefail

version=0.12
prefix=$1
command -v cygpath > /dev/null && prefix=$(cygpath -m "$prefix")
work=$(mktemp -d)
wget -q -O "$work/libmobi.tar.gz" "https://github.com/bfabiszewski/libmobi/releases/download/v$version/libmobi-$version.tar.gz"
echo "9a6fb2c56b916f8fa8b15e0c71008d908109508c944ea1d297881d4e277bf7e7  $work/libmobi.tar.gz" | sha256sum -c -
tar -xf "$work/libmobi.tar.gz" -C "$work"
cd "$work/libmobi-$version"
./configure --prefix="$prefix" --disable-static --without-libxml2 > configure.out
make -j"$(nproc)" -C src > /dev/null
make -C src install > /dev/null
cd /
rm -rf "$work"
