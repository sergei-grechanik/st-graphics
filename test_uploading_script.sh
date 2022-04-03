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
[[ -f _data/butterfly.jpg ]] || \
    curl -o _data/butterfly.jpg "https://upload.wikimedia.org/wikipedia/commons/a/a6/Peacock_butterfly_%28Aglais_io%29_2.jpg"
[[ -f _data/david.jpg ]] || \
    curl -o _data/david.jpg "https://upload.wikimedia.org/wikipedia/commons/8/84/Michelangelo%27s_David_2015.jpg"
[[ -f _data/fern.jpg ]] || \
    curl -o _data/fern.jpg "https://upload.wikimedia.org/wikipedia/commons/3/3d/Giant_fern_forest_7.jpg"
[[ -f _data/nebula.jpg ]] || \
    curl -o _data/nebula.jpg "https://upload.wikimedia.org/wikipedia/commons/b/b1/NGC7293_%282004%29.jpg"
[[ -f _data/flake.jpg ]] || \
    curl -o _data/flake.jpg "https://upload.wikimedia.org/wikipedia/commons/d/d7/Snowflake_macro_photography_1.jpg"
[[ -f _data/flower.jpg ]] || \
    curl -o _data/flower.jpg "https://upload.wikimedia.org/wikipedia/commons/4/40/Sunflower_sky_backdrop.jpg"
[[ -f _data/a_panorama.jpg ]] || \
    curl -o _data/a_panorama.jpg "https://upload.wikimedia.org/wikipedia/commons/thumb/a/ac/Kazbeg_Panorama.jpg/2560px-Kazbeg_Panorama.jpg"
[[ -f _data/column.png ]] || \
    curl -o _data/column.png "https://upload.wikimedia.org/wikipedia/commons/9/95/Column6.png"

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

echo "Wikipedia logo with enforced dpi=96"
$upload_image _data/wikipedia.png --override-dpi 96

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
$upload_image _data/tux.png -r 2 --save-info "$tmpdir/info" -o /dev/null
cat "$tmpdir/info"
image_id="$(grep id "$tmpdir/info" | cut -f 2)"
echo $image_id
$upload_image --show-id "$image_id"

echo "Test saving id and showing id for 256 bit IDs"
$upload_image _data/tux.png --256 -r 2 --save-info "$tmpdir/info" -o /dev/null
image_id="$(grep id "$tmpdir/info" | cut -f 2)"
echo $image_id
(( $image_id < 256 )) || exit 1
$upload_image --show-id "$image_id"

echo "Test saving id and showing id, the id is manually specified"
$upload_image _data/tux.png --id 9999 -r 2 --save-info "$tmpdir/info" -o /dev/null
image_id="$(grep id "$tmpdir/info" | cut -f 2)"
echo $image_id
(( $image_id == 9999 )) || exit 1
$upload_image --show-id "$image_id"

echo "Test saving id and showing id, the id is manually specified < 256"
$upload_image _data/tux.png --id 42 -r 2 --save-info "$tmpdir/info" -o /dev/null
image_id="$(grep id "$tmpdir/info" | cut -f 2)"
echo $image_id
(( $image_id == 42 )) || exit 1
$upload_image --show-id "$image_id"

echo "Testing being verbose (-V) but without status messages (-q)"
$upload_image _data/tux.png --rows 4 -V -q

echo "Test overriding an existing id (the 9999 image above will change)"
$upload_image _data/tux.png --id 9999 -r 4

echo "Test showing part of an image with --show-id"
$upload_image --show-id 9999 --no-upload --rows 2
$upload_image --show-id 9999 --cols 4

echo "A jpeg image"
$upload_image _data/earth.jpg -r 10

echo "Uploading an image using direct method"
$upload_image _data/mars.jpg -r 10 -m direct

echo "Showing an image without uploading"
$upload_image --clear-id 9998 || true
$upload_image _data/saturn.jpg --id 9998 -r 10 --no-upload

echo "Now reuploading the last image using direct method"
$upload_image --fix  9998 -m direct

echo "Deleting and reuploading it again"
$upload_image --clear-id 9998
$upload_image --fix  9998 -m direct

echo "A long image, should be fit to the terminal width"
$upload_image _data/a_panorama.jpg --save-info "$tmpdir/info" -r 20
cat "$tmpdir/info"

echo "A long image, without fitting to terminal width (will look bad)"
$upload_image _data/a_panorama.jpg --save-info "$tmpdir/info" -r 20 --max-cols 1000 -o "$tmpdir/tmp"
cat "$tmpdir/tmp" | head -4

echo "Testing max cols (should be fit to 50 columns)"
$upload_image _data/a_panorama.jpg --save-info "$tmpdir/info" --max-cols 50 -r 20

echo "Testing max rows (only one row)"
$upload_image _data/a_panorama.jpg --save-info "$tmpdir/info" --max-rows 1

echo "The rest of the test will use a temporary cache dir"
export TERMINAL_IMAGES_CACHE_DIR="$tmpdir/cache_dir"

draw_strips() {
    local i=0
    local total_cols=0
    local max_cols="$(tput cols)"
    local opts="$1"
    shift
    for file in "$@"; do
        (( i++ )) || true
        $upload_image "$file" $opts --save-info "$tmpdir/info" -o "$tmpdir/tmp"
        cols="$(grep columns "$tmpdir/info" | cut -f 2)"
        if (( $total_cols + $cols > $max_cols )); then
            paste -d "" "$tmpdir/out_"*
            rm "$tmpdir/out_"*
            total_cols=0
        fi
        (( total_cols += cols )) || true
        mv "$tmpdir/tmp" "$tmpdir/out_$(printf "%02d" $i)"
    done
    paste -d "" "$tmpdir/out_"*
    rm "$tmpdir/out_"*
}

echo "Displaying all images in _data in horizontal strips"

echo "Images are uploaded immediately"
draw_strips "-r 9" _data/*.jpg _data/*.png

echo "Images are uploaded after creating placeholders using --fix"
draw_strips "-r 10 --no-upload" _data/*.jpg _data/*.png
$upload_image --fix

echo "Only 2 images are uploaded after creating placeholders using --fix --last 2"
draw_strips "-r 8 --no-upload" _data/*.png
$upload_image --fix --last 2

export TERMINAL_IMAGES_CACHE_DIR="$tmpdir/cache_dir_test_limits"
export TERMINAL_IMAGES_CLEANUP_PROBABILITY=100

echo "Testing max ids limit"
export TERMINAL_IMAGES_IDS_LIMIT=2
draw_strips "-r 9" _data/*.jpg _data/*.png
$upload_image --status
ids_count="$(ls -1 $TERMINAL_IMAGES_CACHE_DIR/terminals/*-24bit_ids/ | wc -l)"
(( ids_count <= 2 )) || exit 1

echo "Testing max ids limit for 8-bit ids (will probably override old images)"
draw_strips "-r 9 --256" _data/*.jpg _data/*.png
$upload_image --status
ids_count="$(ls -1 $TERMINAL_IMAGES_CACHE_DIR/terminals/*-8bit_ids/ | wc -l)"
(( ids_count <= 2 )) || exit 1

echo "Testing max cache size limit"
export TERMINAL_IMAGES_IDS_LIMIT=
export TERMINAL_IMAGES_CACHE_SIZE_LIMIT=1000
draw_strips "-r 9" _data/*.jpg _data/*.png
$upload_image --status
cache_size="$(du -s $TERMINAL_IMAGES_CACHE_DIR/cache | cut -f1)"
(( $cache_size <= 1000 )) || exit 1
