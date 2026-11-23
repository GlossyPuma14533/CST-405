#!/usr/bin/env bash
# ============================================================================
# Differential fuzzing (make fuzz).  For N random loop programs:
#   compile at -O0, -O1 and -O2, execute each with bench/mipscount.py, and
#   compare with what the reference interpreter in bench/fuzz_gen.py says
#   the program must print.  Any disagreement is a compiler bug; the failing
#   program is kept in bench/out/fuzz_fail_<seed>.cm.
# usage: bash bench/run_fuzz.sh [N]   (default 200)
# ============================================================================
N="${1:-200}"; mkdir -p bench/out; bad=0
for n in $(seq 1 "$N"); do
    python3 bench/fuzz_gen.py "$n" c  > bench/out/fuzz.cm
    expected=$(python3 bench/fuzz_gen.py "$n" py)
    for level in -O0 -O1 -O2; do
        flag="$level"; [ "$level" = -O2 ] && flag=""
        if ! ./minicompiler bench/out/fuzz.cm bench/out/fuzz.s -q $flag > /dev/null 2>&1; then
            echo "  FAIL  seed $n ($level): did not compile"; bad=$((bad+1)); continue
        fi
        got=$(timeout 20 python3 bench/mipscount.py bench/out/fuzz.s 2>&1 | head -n -1 | xargs)
        if [ "$got" != "$expected" ]; then
            echo "  FAIL  seed $n ($level): expected [$expected] got [$got]"
            cp bench/out/fuzz.cm "bench/out/fuzz_fail_$n.cm"; bad=$((bad+1))
        fi
    done
done
echo "  $N random programs x 3 optimization levels: $bad failure(s)"
[ "$bad" -eq 0 ]
