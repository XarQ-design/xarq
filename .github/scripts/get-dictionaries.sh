#!/bin/bash
# Put the English hyphenation and spelling dictionaries in <dir>, for packages that bundle them (the
# AppImage's usr/share/xarq/dictionaries, the Windows folder's dictionaries). They come from LibreOffice's
# dictionaries repository, pinned to a commit and checked, so every package has the same ones.
#
#   get-dictionaries.sh <dir>
set -euo pipefail

dir=$1
base=https://raw.githubusercontent.com/LibreOffice/dictionaries/c0865a85eb67dc377f2cf8e8fe2c4fa9e53f0dcb/en
mkdir -p "$dir"
for f in hyph_en_US.dic en_US.dic en_US.aff README_hyph_en_US.txt README_en_US.txt; do
    wget -q -O "$dir/$f" "$base/$f"
done
(cd "$dir" && sha256sum -c - <<'SUMS'
486fb6840b1049d5d252ef784be11cd4dc2be0bf296568b74b41e4f4e17cba7d  hyph_en_US.dic
f0b1a234bd178bdd01875b2a392a9647f888b8fe879f79c52aae62c2759b3647  en_US.dic
e746c882dd6f303c2c46e7452804b9201115a6942cfeb15f18f8edf774d2e24e  en_US.aff
168b4c01cb841f766a72f310b065c04d09cff46376c37a81d799593ce751c371  README_en_US.txt
9bcae072bff45cc0333d6b3c508c92d85b9065fc861c743c1506b303eba3f4a1  README_hyph_en_US.txt
SUMS
)
