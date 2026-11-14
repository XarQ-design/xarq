#!/bin/bash
# Build GNU LibreDWG into <dir>, for DWG import. Neither Ubuntu nor MSYS2 packages it.
#
#   build-libredwg.sh <dir>
set -euo pipefail

version=0.14
prefix=$1
command -v cygpath > /dev/null && prefix=$(cygpath -m "$prefix")
work=$(mktemp -d)
wget -q -O "$work/libredwg.tar.xz" "https://ftp.gnu.org/gnu/libredwg/libredwg-$version.tar.xz"
echo "62ebb73b984f865960f20ed26619ea5f8789d5e3fd088fa40a2598384da81275  $work/libredwg.tar.xz" | sha256sum -c -
tar -xf "$work/libredwg.tar.xz" -C "$work"
cd "$work/libredwg-$version"
./configure --prefix="$prefix" --disable-static --disable-bindings --disable-docs --disable-python \
    --disable-werror > configure.out
make -j"$(nproc)" -C src > /dev/null
make -C src install > /dev/null
make -C include install > /dev/null
cd /
rm -rf "$work"
