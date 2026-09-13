#!/bin/bash
# Guards the promises this application exists to make: transcription is local,
# nothing reaches the network, and no captured audio or model ever enters the
# repository. Runnable locally and in CI.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
. "$ROOT/tests/platform.sh"
checks=0
failures=0
skipped=0

pass() { checks=$((checks + 1)); echo "  ok   $1"; }
fail() { checks=$((checks + 1)); failures=$((failures + 1)); echo "  FAIL $1"; [ $# -gt 1 ] && echo "       $2"; }
# Counted and reported, so a check that did not run can never be mistaken for
# one that passed.
skip() { skipped=$((skipped + 1)); echo "  skip $1"; [ $# -gt 1 ] && echo "       $2"; }

# ---------------------------------------------------------------- the repository
echo "== Nothing private is tracked =="

# third_party is upstream content this project does not control. Whisper ships
# its own sample media and test fixtures; those are not a user's recording
# committed by accident, which is what these rules exist to catch. The
# distinction only shows up when the dependency is vendored rather than used as
# a submodule, because a parent repository does not list a submodule's files.
tracked_matching() { git ls-files -- "$@" 2>/dev/null | grep -v '^third_party/'; }

found="$(tracked_matching '*.wav' '*.mp3' '*.flac' '*.ogg' '*.m4a')"
[ -z "$found" ] && pass "no captured audio is tracked" \
                || fail "no captured audio is tracked" "$(echo "$found" | head -5 | tr '\n' ' ')"

found="$(tracked_matching '*.bin' '*.gguf' '*.pt' '*.ggml')"
[ -z "$found" ] && pass "no model weights are tracked" \
                || fail "no model weights are tracked" "$(echo "$found" | head -5 | tr '\n' ' ')"

found="$(tracked_matching 'transcription.txt' '*-transcription.txt')"
[ -z "$found" ] && pass "no transcript is tracked" \
                || fail "no transcript is tracked" "$(echo "$found" | head -5 | tr '\n' ' ')"

found="$(git ls-files | grep -E '^(build|build-[^/]*|CMakeFiles)/|/CMakeFiles/|CMakeCache\.txt$|compile_commands\.json$' | head -5)"
[ -z "$found" ] && pass "no build output is tracked" \
                || fail "no build output is tracked" "$(echo "$found" | tr '\n' ' ')"

# A large tracked file is almost always a model or a recording that slipped in.
large="$(git ls-files -z | xargs -0 -r du -k 2>/dev/null | awk '$1 > 2048 {print $2}' | grep -v '^third_party/' | head -5)"
[ -z "$large" ] && pass "no oversized file is tracked outside third_party" \
                || fail "no oversized file is tracked outside third_party" "$(echo "$large" | tr '\n' ' ')"

# ------------------------------------------------------------------ the source
echo "== No networking is introduced =="

OURS="main.cpp gui_main.cpp miniaudio_impl.cpp partial_text.h ui_state.h diagnostics.h
      audio_ring_buffer.h file_storage.h model_info.h speech_detection.h wav_io.h
      audio_player.h transcript_library.h"

found="$(grep -nE '#include[[:space:]]*<(sys/socket|netinet/|arpa/|netdb|curl/|openssl/)' $OURS 2>/dev/null)"
[ -z "$found" ] && pass "no networking headers are included" \
                || fail "no networking headers are included" "$found"

found="$(grep -nE '\b(socket|connect|getaddrinfo|gethostbyname|sendto|recvfrom)[[:space:]]*\(' $OURS 2>/dev/null \
         | grep -vE 'QObject|connect\(|//|\*')"
[ -z "$found" ] && pass "no network calls appear in our sources" \
                || fail "no network calls appear in our sources" "$found"

found="$(grep -niE 'telemetry|analytics|https?://[a-z]' $OURS 2>/dev/null | grep -vE '//.*huggingface|ggerganov')"
[ -z "$found" ] && pass "no telemetry or embedded endpoint" \
                || fail "no telemetry or embedded endpoint" "$found"

# ----------------------------------------------------------------- the binaries
echo "== The built program cannot reach the network =="

BUILD="$(binary_dir "${1:-$ROOT/build-release}")"

# ELF and PE record their imports differently, so the evidence differs even
# though the promise does not.
check_elf_binary() { # path, name
    local binary="$1" name="$2" syms tls direct
    # Mangled C++ names are excluded: Qt's QObject::connect is not connect(2).
    syms="$(nm -D --undefined-only "$binary" 2>/dev/null | awk '{print $2}' | grep -vE '^_Z' \
            | grep -xE 'socket|socketpair|connect|accept|bind|listen|send|sendto|sendmsg|recv|recvfrom|recvmsg|getaddrinfo|gethostbyname|res_query')"
    [ -z "$syms" ] && pass "$name imports no network syscall" \
                   || fail "$name imports no network syscall" "$(echo "$syms" | tr '\n' ' ')"

    # Symbol level, not library level: libcrypto arrives transitively through
    # Qt6Core for hashing and is not something this project chose or uses.
    # What matters is whether we call into TLS or HTTP, and we must not.
    tls="$(nm -D --undefined-only "$binary" 2>/dev/null | awk '{print $2}' \
           | grep -E '^(SSL_|curl_|BIO_s_socket|gnutls_)')"
    [ -z "$tls" ] && pass "$name uses no TLS or HTTP symbol" \
                  || fail "$name uses no TLS or HTTP symbol" "$(echo "$tls" | tr '\n' ' ')"

    # A direct dependency on curl or libssl would mean networking was added.
    direct="$(ldd "$binary" 2>/dev/null | grep -icE 'libcurl|libssl')"
    [ "$direct" = "0" ] && pass "$name links no TLS or HTTP library" \
                        || fail "$name links no TLS or HTTP library" "linked: $direct"
}

check_pe_binary() { # path, name
    local binary="$1" name="$2" imports libraries tls
    if ! command -v dumpbin > /dev/null 2>&1; then
        skip "$name import table" "dumpbin is not on PATH; run from a Developer Command Prompt"
        return
    fi
    # Written -imports rather than /imports so Git Bash does not rewrite the
    # argument into a path. The import table is the whole dependency list.
    imports="$(dumpbin -imports "$binary" 2>/dev/null)"
    if [ -z "$imports" ]; then
        skip "$name import table" "dumpbin produced no output"
        return
    fi

    # On Windows every socket call in every wrapper reaches ws2_32, and every
    # HTTP client reaches wininet or winhttp. Naming the library is stronger
    # evidence here than naming a symbol, because the import table cannot be
    # satisfied any other way without loading a DLL at run time.
    libraries="$(echo "$imports" | grep -ioE '[a-z0-9_-]+\.dll' \
                 | grep -iE '^(ws2_32|wsock32|wininet|winhttp|libcurl|libssl|ssleay32|libeay32)\.dll$')"
    [ -z "$libraries" ] && pass "$name imports no networking library" \
                        || fail "$name imports no networking library" "$(echo "$libraries" | sort -u | tr '\n' ' ')"

    tls="$(echo "$imports" | grep -oE '\b(SSL_[A-Za-z_]+|curl_[a-z_]+|InternetOpen[AW]?|WinHttpOpen)\b')"
    [ -z "$tls" ] && pass "$name uses no TLS or HTTP symbol" \
                  || fail "$name uses no TLS or HTTP symbol" "$(echo "$tls" | sort -u | tr '\n' ' ')"
}

for name in audio_to_text_cli audio_to_text; do
    binary="$BUILD/$name$EXE"
    if [ ! -f "$binary" ]; then
        skip "$name is not built"
        continue
    fi
    if [ "$PLATFORM" = "windows" ]; then
        check_pe_binary "$binary" "$name"
    else
        check_elf_binary "$binary" "$name"
    fi
done

# Strongest available evidence: real work completing with no network at all.
if [ ! -f "$BUILD/audio_to_text_cli$EXE" ] || [ ! -f "$ROOT/models/ggml-base.en.bin" ] \
   || [ ! -f "$ROOT/speech.wav" ]; then
    skip "network namespace test" "needs a built worker, a model and speech.wav"
elif [ "$PLATFORM" != "linux" ]; then
    # Windows has no namespace to drop the process into. A firewall rule is not
    # equivalent: it proves the machine blocked the traffic, not that the
    # program never tried. The claim is unverified there, and says so.
    skip "network namespace test" "no equivalent isolation on $PLATFORM"
elif ! unshare -rn true 2>/dev/null; then
    skip "network namespace test" "unshare is unavailable here"
elif unshare -rn -- "$BUILD/audio_to_text_cli" --benchmark --input "$ROOT/speech.wav" \
       "$ROOT/models/ggml-base.en.bin" --threads 2 2>&1 | grep -q '^PERF|'; then
    pass "a full transcription completes with no network namespace"
else
    fail "a full transcription completes with no network namespace"
fi

echo ""
if [ "$skipped" = "0" ]; then
    echo "$checks check(s), $failures failure(s)"
else
    echo "$checks check(s), $failures failure(s), $skipped skipped"
fi
[ "$failures" = "0" ]
