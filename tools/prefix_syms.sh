#!/bin/sh
# Rename defined globals in a static library so two libretro cores can link together.
# A second run leaves symbols that already have the prefix alone.
set -eu
if [ $# -ne 2 ]; then
	echo "usage: prefix_syms.sh prefix archive.a" >&2
	exit 1
fi
PREFIX=$1
LIB=$2
NM="${DEVKITARM:?}/bin/arm-none-eabi-nm"
OBJCOPY="${DEVKITARM}/bin/arm-none-eabi-objcopy"
MAP=$(mktemp)
"$NM" -g "$LIB" | awk -v p="$PREFIX" '
	function take(name) {
		if (name == "" || index(name, p) == 1) return
		print name, p name
	}
	$2 ~ /^[TBDRCGSVW]$/ { take($3); next }
	$1 ~ /^[TBDRCGSVW]$/ { take($2) }
' | sort -u > "$MAP"
if [ -s "$MAP" ]; then
	"$OBJCOPY" --redefine-syms="$MAP" "$LIB"
fi
rm -f "$MAP"
echo "prefixed $LIB"
