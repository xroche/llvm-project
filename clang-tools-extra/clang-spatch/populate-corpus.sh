#!/bin/bash
# Fills test/Inputs/{cocci,c} with the sample corpus.
#
# The corpus is 1209 semantic patches from the Linux kernel and from
# Coccinelle's own test suite, with their C inputs and, where Coccinelle ships
# them, the expected outputs. They are GPL-2.0 and this tree is Apache-2.0 with
# LLVM exceptions, so they are not committed here. test/Inputs/corpus-manifest.tsv
# is committed, because it is ours, and it names every file this script expects.
#
# Usage: populate-corpus.sh <linux-checkout> <coccinelle-checkout>
set -euo pipefail
LINUX=${1:?path to a Linux kernel checkout}
COCCI=${2:?path to a Coccinelle source checkout}
HERE=$(cd "$(dirname "$0")" && pwd)
IN="$HERE/test/Inputs"

header() { # $1 = destination, $2 = provenance, $3 = project, $4 = licence
  { printf '// clang-spatch test corpus. Copied unmodified below this header.\n'
    printf '// Provenance: %s\n' "$2"
    printf '// Upstream project: %s. Licence: %s.\n' "$3" "$4"
    printf '// Third-party file: do not edit. Line numbers quoted in\n'
    printf '// corpus-manifest.tsv are line numbers in this file, header included.\n'
  } > "$1"
}

copy_tree() { # $1 = source dir, $2 = dest dir, $3 = project, $4 = licence
  local Src=$1 Dst=$2 Project=$3 Licence=$4
  find "$Src" -name '*.cocci' | while read -r F; do
    local Rel=${F#"$Src"/} Out="$Dst/${F#"$Src"/}"
    mkdir -p "$(dirname "$Out")"
    header "$Out" "${Src##*/}/$Rel" "$Project" "$Licence"
    cat "$F" >> "$Out"
  done
}

mkdir -p "$IN/cocci/kernel" "$IN/cocci/coccinelle" "$IN/c"
copy_tree "$LINUX/scripts/coccinelle" "$IN/cocci/kernel" "Linux kernel" "GPL-2.0-only"
for D in tests demos cpptests; do
  [ -d "$COCCI/$D" ] || continue
  copy_tree "$COCCI/$D" "$IN/cocci/coccinelle/$D" "Coccinelle" "GPL-2.0-only"
  # Inputs and expected outputs are copied byte-identical, with no header, so
  # that a .res comparison stays valid.
  mkdir -p "$IN/c/coccinelle/$D"
  # Recursive, because the manifest names nested inputs such as
  # demos/janitorings/bad_zero.c, and a flat copy never produces them.
  find "$COCCI/$D" \( -name '*.c' -o -name '*.cpp' -o -name '*.res' -o -name '*.h' \) \
    -printf '%P\0' | while IFS= read -r -d '' Rel; do
    mkdir -p "$IN/c/coccinelle/$D/$(dirname "$Rel")"
    cp "$COCCI/$D/$Rel" "$IN/c/coccinelle/$D/$Rel"
  done
done
printf 'corpus populated: %s patches, %s inputs\n' \
  "$(find "$IN/cocci" -name '*.cocci' | wc -l)" \
  "$(find "$IN/c" -type f | wc -l)"
