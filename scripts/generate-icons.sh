#!/bin/bash
# Regenerates every icon size from packaging/audio-to-text.png.
#
# The generated files are committed, because a build machine is not required to
# have ImageMagick. Run this only when the artwork itself changes.
#
# Requires: ImageMagick 7 (magick).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE="packaging/audio-to-text.png"
MASTER="packaging/icons/audio-to-text-512.png"
SIZES="16 22 24 32 48 64 128 256"

command -v magick > /dev/null 2>&1 || {
    echo "ImageMagick (magick) is required." >&2
    exit 1
}
[ -f "$SOURCE" ] || { echo "Missing $SOURCE" >&2; exit 1; }

mkdir -p "$(dirname "$MASTER")"

# The artwork is not square and sits off centre. Stretching it to a square would
# distort the glyph, so it is trimmed to its own bounds, scaled to fit, and
# centred on a transparent square with a margin. 440 of 512 leaves roughly 7%
# on each side, which stops the strokes touching the edge at small sizes.
magick "$SOURCE" -trim +repage \
    -resize 440x440 \
    -background none -gravity center -extent 512x512 \
    "$MASTER"

for size in $SIZES; do
    directory="packaging/icons/hicolor/${size}x${size}/apps"
    mkdir -p "$directory"
    # Lanczos keeps the thin strokes legible down to 16 pixels; the default
    # filter turns them to mush.
    magick "$MASTER" -filter Lanczos -resize "${size}x${size}" \
        -strip "$directory/audio-to-text.png"
done

# One file holding every size, which is what Windows expects of an icon.
magick "$MASTER" -define icon:auto-resize=256,128,64,48,32,24,16 \
    packaging/audio-to-text.ico

echo "Generated:"
echo "  $MASTER"
for size in $SIZES; do
    echo "  packaging/icons/hicolor/${size}x${size}/apps/audio-to-text.png"
done
echo "  packaging/audio-to-text.ico"
