#!/bin/bash
# Black-box tests for behaviour that only exists at the process level:
# argument handling, shutdown, and the reporting protocol.
# Usage: cli_tests.sh /path/to/audio_to_text_cli
set -u
# Resolve everything from this script's location so the suite does not depend on
# the working directory. CTest runs it from the build tree.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build-release/audio_to_text_cli}"
MODEL="$ROOT/models/ggml-base.en.bin"
cd "$ROOT" || exit 1
if [ ! -f "$MODEL" ]; then
    echo "SKIP: $MODEL is not present; download a model to run these tests."
    exit 0
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
checks=0
failures=0

check() { # name, condition-result
    checks=$((checks + 1))
    if [ "$2" = "0" ]; then
        echo "  ok   $1"
    else
        failures=$((failures + 1))
        echo "  FAIL $1"
    fi
}

expect_error_category() { # name, category, args...
    local name="$1" category="$2"; shift 2
    local out
    out="$("$BIN" "$@" 2>&1)"
    echo "$out" | grep -q "^ERROR|$category|" && check "$name" 0 || {
        check "$name" 1
        echo "       output: $(echo "$out" | grep '^ERROR|' | head -1)"
    }
}

echo "== Invalid thread counts =="
for bad in 0 -4 abc 99999999999999999999; do
    out="$("$BIN" "$MODEL" --threads "$bad" 2>&1)"
    echo "$out" | grep -qi "thread count must be a positive integer" \
        && check "--threads $bad is rejected" 0 || check "--threads $bad is rejected" 1
done
out="$("$BIN" "$MODEL" --threads 2>&1)"
echo "$out" | grep -qi "missing value after --threads" \
    && check "--threads with no value is rejected" 0 || check "--threads with no value is rejected" 1

echo "== Invalid arguments =="
out="$("$BIN" --duration 7 "$MODEL" 2>&1)"
echo "$out" | grep -qi "duration must be" \
    && check "an unsupported duration is rejected" 0 || check "an unsupported duration is rejected" 1
out="$("$BIN" --nonsense 2>&1)"
echo "$out" | grep -qi "unknown option" \
    && check "an unknown option is rejected" 0 || check "an unknown option is rejected" 1
out="$("$BIN" --device -1 "$MODEL" 2>&1)"
echo "$out" | grep -qi "device index must be" \
    && check "a negative device index is rejected" 0 || check "a negative device index is rejected" 1

echo "== Invalid model paths =="
expect_error_category "a missing model is reported as a model error" "MODEL" "models/definitely-absent.bin"
expect_error_category "a traversal path is refused" "MODEL" "../../etc/passwd"
printf 'not a model' > "$WORK/outside.bin"
expect_error_category "a model outside the approved roots is refused" "MODEL" "$WORK/outside.bin"
expect_error_category "a file that is not a model is refused" "MODEL" "$WORK/outside.bin" --model-dir "$WORK"
expect_error_category "a directory is refused" "MODEL" "$WORK" --model-dir "$WORK"

echo "== Device selection =="
out="$("$BIN" --list-devices 2>&1)"
echo "$out" | grep -q "^DEVICE|0|" && check "devices are enumerated" 0 || check "devices are enumerated" 1
# An index that has gone away no longer stops the recording: falling back to the
# system default is more useful than refusing, and it is reported either way.
out="$(printf 'q\n' | "$BIN" "$MODEL" --device 9999 2>&1)"
echo "$out" | grep -q "^WARN|MICROPHONE|.*no longer available" \
    && check "an out-of-range device falls back to the default" 0 \
    || check "an out-of-range device falls back to the default" 1
echo "$out" | grep -q "Using the system default" \
    && check "the fallback is reported" 0 || check "the fallback is reported" 1

# A monitor source is a loopback of the speakers, and must say so.
out="$("$BIN" --list-devices 2>&1)"
echo "$out" | grep -qE '\|(monitor|microphone)$' \
    && check "devices are labelled as microphone or monitor" 0 \
    || check "devices are labelled as microphone or monitor" 1
echo "$out" | head -1 | grep -q "|microphone$" \
    && check "a real microphone is listed before any monitor" 0 \
    || check "a real microphone is listed before any monitor" 1

echo "== Reporting protocol =="
out="$("$BIN" --privacy 2>&1)"
echo "$out" | grep -q "^DATADIR|" && check "the data directory is reported" 0 || check "the data directory is reported" 1
echo "$out" | grep -qi "no network connections" \
    && check "the privacy notice states local-only processing" 0 \
    || check "the privacy notice states local-only processing" 1

echo "== Worker shutdown =="
FIFO="$WORK/fifo"; LOG="$WORK/log"
mkfifo "$FIFO"
exec 3<>"$FIFO"
"$BIN" "$MODEL" <&3 > "$LOG" 2>&1 &
pid=$!
for _ in $(seq 1 200); do grep -q "^READY|" "$LOG" 2>/dev/null && break; sleep 0.1; done
if grep -q "^READY|" "$LOG" 2>/dev/null; then
    check "the worker reaches its ready prompt" 0
    kill -INT "$pid"
    for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then
        check "SIGINT stops the worker" 1
        kill -KILL "$pid" 2>/dev/null
    else
        wait "$pid"; code=$?
        check "SIGINT stops the worker" 0
        [ "$code" = "0" ] && check "shutdown exits cleanly" 0 || check "shutdown exits cleanly" 1
        grep -q "Shutting down" "$LOG" && check "shutdown is announced" 0 || check "shutdown is announced" 1
    fi
else
    check "the worker reaches its ready prompt" 1
    kill -KILL "$pid" 2>/dev/null
fi
exec 3>&-

# The quit command must also exit cleanly.
printf 'q\n' | "$BIN" "$MODEL" > "$LOG" 2>&1
check "the quit command exits cleanly" $?

echo ""
echo "$checks check(s), $failures failure(s)"
[ "$failures" = "0" ]
