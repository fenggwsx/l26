#!/usr/bin/env bash
# L26 compiler test runner.
#
# Builds the project, then runs three kinds of tests:
#   1. Positive tests  (tests/example*.l26, tests/programs/*.l26,
#      tests/edge/*.l26): compile + run, compare stdout against the
#      matching .expected file. A .in file, if present, is fed on stdin.
#   2. Error tests      (tests/errors/*.l26): must be REJECTED — the
#      compiler must exit nonzero AND print a diagnostic to stderr.
#
# Exit status is nonzero if any test fails.
#
# Usage:  tests/run_tests.sh            # build + run everything
#         tests/run_tests.sh --no-build # skip the build step

set -u

# Resolve directories relative to this script so it works from anywhere.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
L26C="$ROOT_DIR/l26c"

PASS=0
FAIL=0
FAILED_NAMES=()

green() { printf '\033[32m%s\033[0m' "$1"; }
red()   { printf '\033[31m%s\033[0m' "$1"; }

note_pass() { PASS=$((PASS + 1)); printf '  [%s] %s\n' "$(green PASS)" "$1"; }
note_fail() {
    FAIL=$((FAIL + 1))
    FAILED_NAMES+=("$1")
    printf '  [%s] %s\n' "$(red FAIL)" "$1"
    if [ -n "${2:-}" ]; then
        printf '         %s\n' "$2"
    fi
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [ "${1:-}" != "--no-build" ]; then
    echo "== Building =="
    if ! make -C "$ROOT_DIR" >/tmp/l26_build.log 2>&1; then
        echo "BUILD FAILED:"
        cat /tmp/l26_build.log
        exit 1
    fi
    echo "  build ok"
fi

if [ ! -x "$L26C" ]; then
    echo "ERROR: compiler binary not found at $L26C"
    exit 1
fi

# ---------------------------------------------------------------------------
# Positive tests: stdout must equal <name>.expected
# ---------------------------------------------------------------------------
run_positive() {
    local src="$1"
    local name="${src#"$ROOT_DIR"/}"
    local base="${src%.l26}"
    local expected="$base.expected"
    local infile="$base.in"

    if [ ! -f "$expected" ]; then
        note_fail "$name" "missing expected file: ${expected#"$ROOT_DIR"/}"
        return
    fi

    local actual rc
    if [ -f "$infile" ]; then
        actual="$("$L26C" "$src" <"$infile" 2>/tmp/l26_stderr.log)"
        rc=$?
    else
        actual="$("$L26C" "$src" </dev/null 2>/tmp/l26_stderr.log)"
        rc=$?
    fi

    if [ "$rc" -ne 0 ]; then
        note_fail "$name" "compiler exited $rc (stderr: $(head -1 /tmp/l26_stderr.log))"
        return
    fi

    local want
    want="$(cat "$expected")"
    if [ "$actual" == "$want" ]; then
        note_pass "$name"
    else
        note_fail "$name" "stdout mismatch"
        diff <(printf '%s\n' "$want") <(printf '%s\n' "$actual") | sed 's/^/         /'
    fi
}

# ---------------------------------------------------------------------------
# Error tests: must exit nonzero AND emit a diagnostic on stderr
# ---------------------------------------------------------------------------
run_error() {
    local src="$1"
    local name="${src#"$ROOT_DIR"/}"

    local err rc
    err="$("$L26C" "$src" </dev/null 2>&1 >/dev/null)"
    rc=$?

    if [ "$rc" -eq 0 ]; then
        note_fail "$name" "expected rejection but compiler exited 0"
        return
    fi
    if [ -z "$err" ]; then
        note_fail "$name" "exited $rc but produced no diagnostic"
        return
    fi
    if ! printf '%s' "$err" | grep -qi 'error'; then
        note_fail "$name" "diagnostic missing 'error' keyword: $err"
        return
    fi
    note_pass "$name  (rejected: $(printf '%s' "$err" | head -1))"
}

echo
echo "== Positive: examples =="
for f in "$ROOT_DIR"/tests/example*.l26; do
    [ -e "$f" ] && run_positive "$f"
done

echo
echo "== Positive: programs =="
for f in "$ROOT_DIR"/tests/programs/*.l26; do
    [ -e "$f" ] && run_positive "$f"
done

echo
echo "== Positive: edge cases =="
for f in "$ROOT_DIR"/tests/edge/*.l26; do
    [ -e "$f" ] && run_positive "$f"
done

echo
echo "== Error tests (must be rejected) =="
for f in "$ROOT_DIR"/tests/errors/*.l26; do
    [ -e "$f" ] && run_error "$f"
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "================================"
printf 'TOTAL: %d   PASS: %d   FAIL: %d\n' "$((PASS + FAIL))" "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    echo "Failed tests:"
    for n in "${FAILED_NAMES[@]}"; do
        echo "  - $n"
    done
    exit 1
fi
echo "All tests passed."
exit 0
