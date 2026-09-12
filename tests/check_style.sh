#!/bin/bash
# Deterministic formatting checks. These are the rules a reviewer would enforce
# by eye, made mechanical: no clang-format dependency, and no risk of a tool
# version reformatting the tree differently from the one used here.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
LIMIT=120
checks=0
failures=0

pass() { checks=$((checks + 1)); echo "  ok   $1"; }
fail() { checks=$((checks + 1)); failures=$((failures + 1)); echo "  FAIL $1"; [ $# -gt 1 ] && echo "$2"; }

FILES="main.cpp gui_main.cpp partial_text.h ui_state.h diagnostics.h audio_ring_buffer.h
       file_storage.h model_info.h speech_detection.h wav_io.h
       tests/unit_tests.cpp tests/gui_tests.cpp tests/test_harness.h"

found="$(grep -lP '\t' $FILES 2>/dev/null)"
[ -z "$found" ] && pass "indentation uses spaces, never tabs" \
                || fail "indentation uses spaces, never tabs" "$(echo "$found" | sed 's/^/       /')"

found="$(grep -nE '[[:space:]]+$' $FILES 2>/dev/null | head -5)"
[ -z "$found" ] && pass "no trailing whitespace" \
                || fail "no trailing whitespace" "$(echo "$found" | sed 's/^/       /')"

found="$(grep -lP '\r$' $FILES 2>/dev/null)"
[ -z "$found" ] && pass "line endings are LF" \
                || fail "line endings are LF" "$(echo "$found" | sed 's/^/       /')"

missing=""
for file in $FILES; do
    [ -n "$(tail -c1 "$file")" ] && missing="$missing $file"
done
[ -z "$missing" ] && pass "every file ends with a newline" \
                  || fail "every file ends with a newline" "      $missing"

long="$(awk -v limit="$LIMIT" 'length > limit {print FILENAME ":" FNR " is " length " columns"}' $FILES | head -5)"
[ -z "$long" ] && pass "no line exceeds $LIMIT columns" \
               || fail "no line exceeds $LIMIT columns" "$(echo "$long" | sed 's/^/       /')"

# Deliberately not checked here: alignment of continuation lines, brace
# placement and argument wrapping. Distinguishing a correctly aligned
# continuation from a misindented statement needs a real parser, and a
# half-rule that flags good code is worse than no rule. A .clang-format file
# is provided for editors; adopting it as a gate needs a one-time
# normalisation commit that should not ride along inside a CI change.

echo ""
echo "$checks check(s), $failures failure(s)"
[ "$failures" = "0" ]
