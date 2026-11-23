#!/usr/bin/env bash
# ============================================================================
# CST-405 Topic 4 — optimized vs unoptimized benchmark     (used by `make bench`)
# ----------------------------------------------------------------------------
# For each benchmark program:
#   1. compile three times:  -O0 (no optimization at all), -O1 (the TAC
#      optimizer only) and the default -O2 (TAC optimizer + back-end peepholes)
#   2. run all three under SPIM and check they print the expected output
#   3. count what each version EXECUTES with bench/mipscount.py
# and print one table.  The counts are machine independent: every executed
# assembly instruction counts once (a SPIM pseudo-instruction such as mul or
# sle counts as one), loads/stores are memory traffic (spills and reloads).
# ============================================================================
# Run from the compiler/ directory.
BENCHES="${@:-tests/t4_10_bench_hotloop.cm tests/t4_15_bench_sieve.cm tests/t4_16_bench_nested.cm tests/t4_06_optimize.cm}"
mkdir -p bench/out

field() { echo "$1" | tr ' ' '\n' | grep "^$2=" | cut -d= -f2; }

printf "\n  %-24s %-6s %8s %8s %8s %8s %9s\n" "program" "build" "TAC" "MIPS" "executed" "ld+st" "time(ms)"
printf "  %s\n" "──────────────────────────────────────────────────────────────────────────────"
for f in $BENCHES; do
    name=$(basename "$f" .cm)
    expected=$(grep -m1 -o "Expected output:[-0-9 ]*" "$f" | sed 's/Expected output://' | xargs)
    for build in O0 O1 O2; do
        s="bench/out/$name.$build.s"
        flag="-$build"; [ "$build" = O2 ] && flag=""
        ./minicompiler "$f" "$s" -q $flag > bench/out/$name.$build.log 2>&1 || { echo "  $name: compile failed"; continue; }
        tac=$(grep -c "^ *[0-9]*:" "${s%.s}.optimized.tac")
        mips=$(grep -cE "^    [a-z]" "$s")
        got=$(spim -file "$s" 2>/dev/null | grep -v -E '^(SPIM Version|Copyright|All Rights|See the file|Loaded:)' | xargs)
        [ "$got" = "$expected" ] || echo "  !! $name ($build) printed [$got], expected [$expected]"
        stats=$(python3 bench/mipscount.py "$s" --quiet)
        ex=$(field "$stats" instructions); ld=$(field "$stats" loads); st=$(field "$stats" stores)
        t0=$(date +%s%N); spim -file "$s" > /dev/null 2>&1; t1=$(date +%s%N)
        ms=$(( (t1 - t0) / 1000000 ))
        printf "  %-24s %-6s %8s %8s %8s %8s %9s\n" "$name" "$build" "$tac" "$mips" "$ex" "$((ld+st))" "$ms"
        eval "EX_$build=$ex"
    done
    if [ -n "$EX_O0" ] && [ -n "$EX_O2" ] && [ "$EX_O0" -gt 0 ]; then
        printf "  %-24s %-6s %44s\n" "" "" "$(awk -v a=$EX_O0 -v b=$EX_O2 'BEGIN{printf "O2 vs O0: %.1f%% fewer executed, %.2fx", 100*(a-b)/a, a/b}')"
    fi
    printf "  %s\n" "──────────────────────────────────────────────────────────────────────────────"
done
echo "  TAC = instructions in the .optimized.tac the back end used"
echo "  MIPS = instructions in the .s file;  executed / ld+st = dynamic counts"
echo "  time = wall-clock SPIM run (start-up dominated; shown for completeness)"
