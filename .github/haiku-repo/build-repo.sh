#!/bin/bash
# Builds the XarQ package repository for Haiku from a folder of .hpkg files:
#     .github/haiku-repo/build-repo.sh <packages-dir> <output-dir>
# Takes every .hpkg in the folder: XarQ's own (XarQ-26.10-alpha-haiku-x86_64.hpkg)
# and those of the libraries it needs (jxrlib-1.2-1-x86_64.hpkg). Each architecture
# gets <output-dir>/<arch>/current/ with packages/, repo, repo.info and repo.sha256,
# which is what `pkgman add-repo https://hpkg.xarq.org` reads (x86_64; the site serves
# that folder, which also stays at https://xarq.org/haiku/x86_64/current).
# package_repo comes from the HaikuPorts build image, so this runs on Linux.
set -euo pipefail

PACKAGES=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
BASE_URL=${BASE_URL:-https://xarq.org/haiku}
HPKG_URL=${HPKG_URL:-https://hpkg.xarq.org}
# Pinned: the image has no "latest" tag.
IMAGE=${HAIKU_TOOLS_IMAGE:-ghcr.io/haikuports/haikuporter/buildmaster:1.5.3}
ENGINE=${CONTAINER_ENGINE:-docker}
# Must match the vendor written in every package.
VENDOR=${REPO_VENDOR:-XarQ}

shopt -s nullglob
files=("$PACKAGES"/*.hpkg)
if [ ${#files[@]} -eq 0 ]; then
	echo "No .hpkg files in $PACKAGES" >&2
	exit 1
fi

# pkgman downloads packages/<name>-<version>-<arch>.hpkg, so each file is copied
# under that name, read from the package itself rather than the release file name.
info=$("$ENGINE" run --rm -v "$PACKAGES":/in:Z,ro -e LD_LIBRARY_PATH=/usr/local/lib "$IMAGE" sh -c '
	for f in /in/*.hpkg; do
		package list -i "$f" | awk -v f="${f#/in/}" "
			/^\tname: / { n = \$2 } /^\tversion: / { v = \$2 } /^\tarchitecture: / { a = \$2 }
			END { print f, n, v, a }"
	done')

# A repository holds one version of each package: keep the newest of each name and architecture (sort -V
# orders 26.10~alpha before 26.11~alpha, as Haiku does).
info=$(sort -k2,2 -k4,4 -k3,3V <<< "$info" | awk '{ newest[$2 " " $4] = $0 } END { for (k in newest) print newest[k] }')

rm -rf "$OUT"/*/current
while read -r file name version arch; do
	if [ -z "$arch" ]; then
		echo "Could not read the package info of $file" >&2
		exit 1
	fi
	mkdir -p "$OUT/$arch/current/packages"
	cp "$PACKAGES/$file" "$OUT/$arch/current/packages/$name-$version-$arch.hpkg"
done <<< "$info"

for dir in "$OUT"/*/current; do
	arch=$(basename "$(dirname "$dir")")
	case $arch in
	x86_64) baseurl=$HPKG_URL ;;
	*) baseurl=$BASE_URL/$arch/current ;;
	esac

	# The identifier must never change: Haiku uses it to tell repositories apart,
	# and changing it makes every user remove and add the repository again.
	cat > "$dir/repo.info" <<EOF
name XarQ
vendor "$VENDOR"
summary "XarQ, the vector illustration and page layout application, and the libraries it needs"
priority 2
baseurl $baseurl
identifier tag:xarq.org,2026:repositories/xarq/$arch
architecture $arch
EOF

	if ! "$ENGINE" run --rm -v "$dir":/repo:Z -w /repo -e LD_LIBRARY_PATH=/usr/local/lib "$IMAGE" \
			sh -c 'package_repo create repo.info packages/*.hpkg'; then
		echo "package_repo failed for $arch: see its message above. A package whose vendor isn't \"$VENDOR\"" >&2
		echo "is one cause: build with VENDOR=\"$VENDOR\" in haikuports.conf, or with src/haiku/make-hpkg.sh." >&2
		exit 1
	fi
	sha256sum "$dir/repo" | cut -d' ' -f1 > "$dir/repo.sha256"
	echo "$arch: $(ls "$dir/packages" | wc -l) package(s) in $dir"
done
