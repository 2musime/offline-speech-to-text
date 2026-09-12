#!/bin/bash
# Downloads a Whisper model into a directory the application will accept.
#
# This is the only step that touches the network, and it is deliberately
# separate from transcription, which never does.
set -u
usage() {
    cat <<USAGE
Usage: install-model.sh [MODEL] [--system|--user|--dir PATH]

  MODEL     base.en (default), small.en, tiny.en, medium.en, large-v3
  --user    install to \$XDG_DATA_HOME/audio-to-text/models  (default)
  --system  install to /usr/share/audio-to-text/models       (needs root)
  --dir P   install to P

Models come from the whisper.cpp project's own distribution at
https://huggingface.co/ggerganov/whisper.cpp

Record the checksum after downloading and verify it later:
  sha256sum <model> >> SHA256SUMS && sha256sum --check SHA256SUMS
USAGE
}

MODEL="base.en"
DEST=""
while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --user)    DEST="${XDG_DATA_HOME:-$HOME/.local/share}/audio-to-text/models"; shift ;;
        --system)  DEST="/usr/share/audio-to-text/models"; shift ;;
        --dir)     DEST="${2:-}"; shift 2 ;;
        -*)        echo "Unknown option: $1" >&2; usage; exit 1 ;;
        *)         MODEL="$1"; shift ;;
    esac
done
[ -n "$DEST" ] || DEST="${XDG_DATA_HOME:-$HOME/.local/share}/audio-to-text/models"

case "$MODEL" in
    tiny.en|base.en|small.en|medium.en|large-v3) ;;
    *) echo "Unsupported model: $MODEL" >&2; usage; exit 1 ;;
esac

SCRIPT="$(cd "$(dirname "$0")/.." && pwd)/third_party/whisper.cpp/models/download-ggml-model.sh"
if [ ! -f "$SCRIPT" ]; then
    echo "Missing $SCRIPT" >&2
    echo "Run: git submodule update --init --recursive" >&2
    exit 1
fi

mkdir -p "$DEST" || { echo "Cannot create $DEST" >&2; exit 1; }
echo "Downloading $MODEL into $DEST"
bash "$SCRIPT" "$MODEL" "$DEST" || exit 1

FILE="$DEST/ggml-$MODEL.bin"
[ -f "$FILE" ] || { echo "Download did not produce $FILE" >&2; exit 1; }
chmod 0644 "$FILE"
echo ""
echo "Installed: $FILE"
echo "Size:      $(du -h "$FILE" | cut -f1)"
echo "SHA-256:   $(sha256sum "$FILE" | cut -d' ' -f1)"
echo ""
echo "Verify it loads:"
echo "  audio_to_text_cli \"$FILE\" --model-dir \"$DEST\" --version"
