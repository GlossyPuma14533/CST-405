#!/usr/bin/env bash
# ============================================================================
# CST-405 Topic 4 — automatic regression check          (used by `make check`)
# ----------------------------------------------------------------------------
# Every test program states what it should do in its header comment:
#   /* Expected output: 14  20  9 ...      -> compile, run under SPIM, and the
#                                             printed numbers must match
#   /* THIS PROGRAM IS SUPPOSED TO FAIL.   -> the compiler must REJECT it
#                                             (exit code 1, no assembly run)
# Every program in tests_neg/ must be rejected, whatever its header says.
#
# Each passing program is checked at ALL THREE optimization levels: -O0 (none),
# -O1 (TAC optimizer only) and the default -O2.  All must print the same,
# correct numbers -- so an optimization that changes what a program means
# fails the suite.
# This script prints PASS/FAIL per test plus a total, so "all tests pass" is
# a fact you can show, not a claim.
# ============================================================================
# Run from the compiler/ directory (make check does this for you).
pass=0; fail=0

run_spim() {   # program output only, SPIM's banner lines dropped
    spim -file "$1" 2>/dev/null \
        | grep -v -E '^(SPIM Version|Copyright|All Rights|See the file|Loaded:)' | xargs
}

must_fail() {
    if ./minicompiler "$1" "${1%.cm}.s" -q > /dev/null 2>&1; then
        printf "  FAIL  %-44s compiled, but should have been rejected\n" "$1"; fail=$((fail+1))
    else
        printf "  PASS  %-44s rejected, as expected\n" "$1"; pass=$((pass+1))
    fi
}

for f in tests/*.cm tests_neg/*.cm; do
    [ -e "$f" ] || continue
    case "$f" in tests_neg/*) must_fail "$f"; continue ;; esac
    if grep -q "SUPPOSED TO FAIL" "$f"; then must_fail "$f"; continue; fi

    expected=$(grep -m1 -o "Expected output:[-0-9 ]*" "$f" | sed 's/Expected output://' | xargs)
    s="${f%.cm}.s"; s0="${f%.cm}.O0.s"; s1="${f%.cm}.O1.s"
    if ! ./minicompiler "$f" "$s" -q > /dev/null 2>&1; then
        printf "  FAIL  %-44s did not compile\n" "$f"; fail=$((fail+1)); continue
    fi
    if ! ./minicompiler "$f" "$s0" -q -O0 > /dev/null 2>&1 ||
       ! ./minicompiler "$f" "$s1" -q -O1 > /dev/null 2>&1; then
        printf "  FAIL  %-44s did not compile with -O0 / -O1\n" "$f"; fail=$((fail+1)); continue
    fi
    actual=$(run_spim "$s"); actual0=$(run_spim "$s0"); actual1=$(run_spim "$s1")
    if [ "$actual" != "$expected" ]; then
        printf "  FAIL  %-44s expected [%s] got [%s]\n" "$f" "$expected" "$actual"; fail=$((fail+1))
    elif [ "$actual0" != "$expected" ]; then
        printf "  FAIL  %-44s -O0 build: expected [%s] got [%s]\n" "$f" "$expected" "$actual0"; fail=$((fail+1))
    elif [ "$actual1" != "$expected" ]; then
        printf "  FAIL  %-44s -O1 build: expected [%s] got [%s]\n" "$f" "$expected" "$actual1"; fail=$((fail+1))
    else
        printf "  PASS  %-44s %s\n" "$f" "$actual"; pass=$((pass+1))
    fi
done
echo "  ─────────────────────────────────────────────────────────"
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
