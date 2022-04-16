#!/bin/bash
# An example script that show multiple images
# Usage:
#   show-multiple-images.sh <row_number> <images>

upload_image="$(dirname $0)/upload-terminal-image.sh"

tmpdir="$(mktemp -d)"
if [[ -z "$tmpdir" ]]; then
    exit 1
fi

cleanup() {
    rm -r "$tmpdir"
}
trap cleanup EXIT TERM

i=0
total_cols=0
max_cols="$(tput cols)"
rows="$1"
shift
for file in "$@"; do
    (( i++ )) || true
    $upload_image "$file" -r "$rows" --save-info "$tmpdir/info" -o "$tmpdir/tmp" || exit 1
    cols="$(grep columns "$tmpdir/info" | cut -f 2)"
    if (( $total_cols + $cols > $max_cols )); then
        paste -d "" "$tmpdir/out_"* || exit 1
        rm "$tmpdir/out_"*
        total_cols=0
    fi
    (( total_cols += cols )) || true
    mv "$tmpdir/tmp" "$tmpdir/out_$(printf "%02d" $i)"
done
paste -d "" "$tmpdir/out_"* || exit 1
rm "$tmpdir/out_"*
