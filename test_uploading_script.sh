#!/bin/bash

set -e

upload_image="$1"

mkdir _data 2> /dev/null || true

[[ -f _data/wikipedia.png ]] || \
    curl -o _data/wikipedia.png https://upload.wikimedia.org/wikipedia/en/thumb/8/80/Wikipedia-logo-v2.svg/440px-Wikipedia-logo-v2.svg.png
[[ -f _data/transparency.png ]] || \
    curl -o _data/transparency.png https://upload.wikimedia.org/wikipedia/commons/4/47/PNG_transparency_demonstration_1.png
[[ -f _data/tux.png ]] || \
    curl -o _data/tux.png https://upload.wikimedia.org/wikipedia/commons/a/af/Tux.png
[[ -f _data/earth.jpg ]] || \
    curl -o _data/earth.jpg "https://upload.wikimedia.org/wikipedia/commons/thumb/c/cb/The_Blue_Marble_%28remastered%29.jpg/240px-The_Blue_Marble_%28remastered%29.jpg"
[[ -f _data/mars.jpg ]] || \
    curl -o _data/mars.jpg "https://upload.wikimedia.org/wikipedia/commons/0/02/OSIRIS_Mars_true_color.jpg"
[[ -f _data/jupiter.jpg ]] || \
    curl -o _data/jupiter.jpg "https://upload.wikimedia.org/wikipedia/commons/2/2b/Jupiter_and_its_shrunken_Great_Red_Spot.jpg"
[[ -f _data/saturn.jpg ]] || \
    curl -o _data/saturn.jpg "https://upload.wikimedia.org/wikipedia/commons/c/c7/Saturn_during_Equinox.jpg"
[[ -f _data/sun.jpg ]] || \
    curl -o _data/sun.jpg "https://upload.wikimedia.org/wikipedia/commons/thumb/b/b4/The_Sun_by_the_Atmospheric_Imaging_Assembly_of_NASA%27s_Solar_Dynamics_Observatory_-_20100819.jpg/628px-The_Sun_by_the_Atmospheric_Imaging_Assembly_of_NASA%27s_Solar_Dynamics_Observatory_-_20100819.jpg"

tmpdir="$(mktemp -d)"
if [[ -z "$tmpdir" ]]; then
    exit 1
fi

cleanup() {
    rm -r "$tmpdir"
}
trap cleanup EXIT TERM

echo "Just wikipedia logo"
$upload_image _data/wikipedia.png

echo "Testing -r (5 images)"
for i in $(seq 1 5); do
    $upload_image _data/wikipedia.png -r $i
done
echo "Testing -c (5 images)"
for i in $(seq 1 5); do
    $upload_image _data/wikipedia.png -c $i
done

echo "Testing -r and -c and output to a file (4 x 6 images)"
for rows in $(seq 1 4); do
    for cols in $(seq 1 6); do
        $upload_image _data/tux.png -r $rows -c $cols -o "$tmpdir/cols_$cols"
    done
    paste -d "" "$tmpdir"/cols_*
    rm "$tmpdir"/cols_*
done

echo "Testing appending to a file (4 images)"
for n in $(seq 2 5); do
    $upload_image _data/tux.png --rows $n --columns $(( $n * 2 )) -a -o "$tmpdir/img"
done
cat "$tmpdir/img"

echo "Test saving id and showing id"
$upload_image _data/tux.png -r 2 --save-id "$tmpdir/id" -o /dev/null
cat "$tmpdir/id"
$upload_image --show-id "$(cat "$tmpdir/id")"
echo "Test saving id and showing id for 256 bit IDs"
$upload_image _data/tux.png --256 -r 2 --save-id "$tmpdir/id" -o /dev/null
cat "$tmpdir/id"
(( $(cat "$tmpdir/id") < 256 )) || exit 1
$upload_image --show-id "$(cat "$tmpdir/id")"
echo "Test saving id and showing id, the id is manually specified"
$upload_image _data/tux.png --id 9999 -r 2 --save-id "$tmpdir/id" -o /dev/null
cat "$tmpdir/id"
(( $(cat "$tmpdir/id") == 9999 )) || exit 1
$upload_image --show-id "$(cat "$tmpdir/id")"
echo "Test saving id and showing id, the id is manually specified < 256"
$upload_image _data/tux.png --id 42 -r 2 --save-id "$tmpdir/id" -o /dev/null
cat "$tmpdir/id"
(( $(cat "$tmpdir/id") == 42 )) || exit 1
$upload_image --show-id "$(cat "$tmpdir/id")"

echo "Testing being verbose (-V) but without status messages (-q)"
$upload_image _data/tux.png --rows 4 -V -q

echo "Test overriding an existing id (the 9999 image above will change)"
$upload_image _data/tux.png --id 9999 -r 4

echo "Test showing part of the image with --show-id"
$upload_image --show-id 9999 --no-upload --rows 2
$upload_image --show-id 9999 --cols 4

echo "A jpeg image"
$upload_image _data/earth.jpg -r 10

echo "Uploading an image using direct method"
$upload_image _data/mars.jpg -r 10 -m direct

echo "Showing an image without uploading"
$upload_image _data/jupiter.jpg --id 9998 -r 10 --no-upload

echo "Now reuploading the last image"
$upload_image --fix  9998 -m direct
