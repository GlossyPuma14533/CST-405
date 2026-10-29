#!/usr/bin/env bash
# ============================================================================
# CST-405 Topic 3 — automatic regression check          (used by `make check`)
# ----------------------------------------------------------------------------
# Every test program states what it should do in its header comment:
#   /* Expected output: 14  20  9 ...      -> compile, run under SPIM, and the
#                                             printed numbers must match
#   /* THIS PROGRAM IS SUPPOSED TO FAIL.   -> the compiler must REJECT it
#                                             (exit code 1, no assembly run)
# This script reads that header, does the check, and prints PASS/FAIL per
# test plus a total — so "all tests pass" is a fact you can show, not a claim.
# ============================================================================
# Run from the compiler/ directory (make check does this for you).
pass=0; fail=0
for f in tests/*.cm; do
    s="${f%.cm}.s"
    if grep -q "SUPPOSED TO FAIL" "$f"; then
        if ./minicompiler "$f" "$s" -q > /dev/null 2>&1; then
            printf "  FAIL  %-40s compiled, but should have been rejected\n" "$f"; fail=$((fail+1))
        else
            printf "  PASS  %-40s rejected, as expected\n" "$f"; pass=$((pass+1))
        fi
        continue
    fi
    expected=$(grep -m1 -o "Expected output:[-0-9 ]*" "$f" | sed 's/Expected output://' | xargs)
    if ! ./minicompiler "$f" "$s" -q > /dev/null 2>&1; then
        printf "  FAIL  %-40s did not compile\n" "$f"; fail=$((fail+1)); continue
    fi
    # Drop SPIM's banner lines; keep only what the program printed.
    actual=$(spim -file "$s" 2>/dev/null \
             | grep -v -E '^(SPIM Version|Copyright|All Rights|See the file|Loaded:)' | xargs)
    if [ "$actual" == "$expected" ]; then
        printf "  PASS  %-40s %s\n" "$f" "$actual"; pass=$((pass+1))
    else
        printf "  FAIL  %-40s expected [%s] got [%s]\n" "$f" "$expected" "$actual"; fail=$((fail+1))
    fi
done
echo "  ─────────────────────────────────────────────"
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
