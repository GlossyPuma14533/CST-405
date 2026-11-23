# CST-405 · Topic 4 — Compiling Loops

**Weeks 9–11 · Nov 2 – Nov 22, 2026**

Control flow becomes labels and jumps; then the optimizer earns its keep.

---

## What you are given

This folder is a **working compiler**. Build it before you change anything:

```bash
cd compiler
make
```

- the complete Topic 3 compiler

## What you are building

- relational operators, and their place in the precedence table
- `while` and `for`, lowered to labels and jumps
- `break`, with the label stack and the depth check
- label and branch emission in the back end
- branch simplification in the optimizer

Every place you need to write something is marked `TODO (Topic 4)` in the source,
with the guidance you need at that spot. Work through them in the order they appear
in the file — they are ordered deliberately.

## Building and testing

```bash
cd compiler
make                                   # build ./minicompiler
make test                              # compile and run every tests/*.cm
make clean                             # remove everything make produced

./minicompiler tests/t4_06_optimize.cm out.s          # full trace of all six phases
./minicompiler tests/t4_06_optimize.cm out.s -q       # quiet: errors and summary only
spim -file out.s                       # run the generated MIPS
```

Compiling also writes `out.tac` and `out.optimized.tac`. Reading those two side by
side is the fastest way to see what the optimizer did.

## You are done when

- all six `t4_*` tests pass, plus everything from Topics 2 and 3
- the optimizer reports a scorecard and reaches a fixed point
- a benchmark has been measured optimized vs unoptimized

## Requirements

`flex`, `bison`, `gcc`, `make`, and [SPIM](http://spimsimulator.sourceforge.net/)
or QtSPIM to run the generated assembly. `python3` for `make bench` / `make fuzz`.

---

# Our implementation (Project 4)

**Team:** Christoffer Hansen, Emma Schaffner. Who did what is in
[`TEAM_CONTRIBUTIONS.md`](TEAM_CONTRIBUTIONS.md).

Every `TODO (Topic 4)` is implemented (`grep -rn "TODO (Topic 4)" compiler`
now only finds the "has been implemented" status lines). Every Topic 2 and
Topic 3 feature that the Topic 4 starter had dropped has been brought back.

```bash
cd compiler
make clean && make        # -Wall, no warnings; bison reports 0 conflicts
make check                # 44 programs, PASS/FAIL, each at -O0, -O1 and -O2  -> 44 passed, 0 failed
make bench                # optimized vs unoptimized benchmark table
make fuzz                 # 200 random loop programs x 3 levels vs a reference -> 0 failures
```

## Checklist

| Requirement | Where | Status |
|---|---|---|
| All `t4_*` tests pass, plus Topics 2 and 3 | `make check` | **44 passed, 0 failed** (11 Topic 2, 13 Topic 3, 16 Topic 4, 4 Topic 4 negative) |
| At least one type of loop | `while`, `for`, `break` | both loop kinds, all `for` header parts optional, nested to any depth |
| Order of operations | `parser.y` precedence table | relational ops below `+ -` below `* /` below unary minus; tests `t4_08`, `t3_01`, `t3_09` |
| Optimizer reports a scorecard and reaches a fixed point | `tac.c` `optimizeTAC` | 11 techniques, counted per technique, repeated until a pass makes no change |
| Benchmark optimized vs unoptimized | `make bench` | up to **2.41× fewer instructions executed** (table below) |
| Lexical analyzer | `scanner.l` | lexical errors are fatal, with their line |
| Syntax analyzer, AST, symbol table, syntax errors with location | `parser.y`, `ast.c`, `symtab.c` | precise "missing `;`" messages, now including `break` |
| Semantic analyzer with located errors | `semantic.c` | `break` outside a loop, plus every Topic 3 check |
| 3-address code saved to a file | `tac.c` | `out.tac` and `out.optimized.tac` |
| Flow and peephole optimization | `tac.c`, `codegen.c` | branch simplification, unreachable code, jump-to-next, copy coalescing, compare-and-branch fusion, immediates |
| MIPS code saved to a file, runs in SPIM / QtSPIM | `codegen.c` | `out.s` |

## Running on Ubuntu / WSL (step by step)

```bash
sudo apt update && sudo apt install -y build-essential flex bison spim python3
cd "Project 4/compiler"
make clean && make
make check                                        # automatic regression suite
./minicompiler tests/t4_10_bench_hotloop.cm out.s # full six-phase trace + scorecard
diff out.tac out.optimized.tac                    # what the optimizer did
spim -file out.s                                  # -> 10100
./minicompiler tests/t4_10_bench_hotloop.cm out0.s -q -O0   # the unoptimized build
make bench                                        # measure the difference
```

Options: `-q` quiet · `-O0` no optimization at all · `-O1` TAC optimizer only ·
(no flag) `-O2`, the TAC optimizer plus the back-end peepholes.

## What was added, phase by phase

| Phase | File | Topic 4 work |
|---|---|---|
| 1 Lexical | `scanner.l` | `while` `for` `break`; `< > <= >= == !=` (two-character operators as their own rules, so longest-match keeps `<=` one token). **Restored:** fatal lexical errors with their line, `\r` as whitespace, `yylloc` for error lines |
| 2 Syntax | `parser.y` | `while (c) s`, `for (init; cond; update) s` with every part optional, `break;`; relational operators in the precedence table. **0 conflicts.** **Restored:** file-scope statements (now including loops) forming an implicit `main`, deferred syntax errors, "missing `;`" error productions, now with one for `break` |
| 2 AST | `ast.h`, `ast.c` | `NODE_WHILE`, `NODE_FOR`, `NODE_BREAK`; relational operators packed into `BINOP` (`l g e n` for `<= >= == !=`, see `opText`); `printAST` cases |
| 3 Semantic | `semantic.c` | `break` depth check: only the loop BODY counts, and break is lexical, so a function called from a loop cannot break it. **Restored:** exactly one entry point, `main` takes no parameters, duplicate and reserved (`__`) parameters, indexing a scalar, an array used as a value (including as a loop condition), assigning a whole array, scalar passed for an `int[]`, at most 4 parameters, params and body sharing one scope. **Fixed:** in a call with 3+ arguments the first ones were never checked (an undeclared name crashed the back end) |
| 4 TAC | `tac.c` | `LABEL`, `GOTO`, `IF_FALSE`; `while` and `for` lowered with the test at the top; a break-label stack. **Restored:** scope renaming (`i__1`), so a variable declared in a loop body gets its own slot; the implicit `main`. **New:** strictly left-to-right evaluation |
| 5 Optimizer | `tac.c` | branch simplification and unreachable-code removal; then four loop-aware techniques and two peepholes (next table) |
| 6 MIPS | `codegen.c` | labels; `j`; `beq`/`bne`; `slt sgt sle sge seq sne`; the register cache is written back at every label and branch. **Restored:** register pinning; `return` writes back only globals. **New:** back-end peepholes (below) |

## How loops are lowered

```
while (c) B                     for (init; c; upd) B
──────────────────────────      ──────────────────────────────
Lstart:                             <init>
    t = c                       Lstart:
    IF_FALSE t GOTO Lend            t = c            (omitted if c is empty)
    B                               IF_FALSE t GOTO Lend
    GOTO Lstart                     B
Lend:                               <upd>
                                    GOTO Lstart
break  ->  GOTO <exit label of  Lend:
           the innermost loop>
```

The exit label is pushed on a **break-label stack** while the body is
generated and popped afterwards. So `break` always jumps to the innermost
loop, and nesting needs no special case (`t4_09_nested_break`).

Real output for `tests/t4_03_break.cm` (`out.tac` → `out.optimized.tac`,
**27 → 17 instructions**): the `GOTO Lstart` after each `break` is
unreachable and removed. The second loop's body was only `break`, so its
`GOTO L3` lands on the very next label, and the test itself goes too:

```
 L0:  t0 = i < 100              L0:  t0 = i < 100
      IF_FALSE t0 GOTO L1            IF_FALSE t0 GOTO L1
      t0 = i + 7                     i = i + 7          <- copy coalesced
      i = t0                    L1:  ...
      GOTO L1                        i = 0              <- 2nd "i = 0" removed
      GOTO L0      <- unreachable L2:
 L1:  ...                       L3:  t0 = i + 4
      i = 0                          ...
      i = 0
 L2:  t0 = i < 100              (whole 2nd loop test gone: both
      IF_FALSE t0 GOTO L3        outcomes reach L3)
      GOTO L3
      t0 = i + 1   <- unreachable
      ...
```

## The optimizer

`optimizeTAC` runs passes until one makes **no change** (a fixed point). A
second pass is needed because one rewrite exposes another: a folded condition
becomes a `GOTO`, and only the next pass removes the code that `GOTO` made
unreachable. The scorecard counts every technique:

| # | Technique | Example |
|---|---|---|
| 1 | Algebraic simplification | `x + 0`, `x * 1`, `x * 0`, `x / 1` |
| 2 | Constant folding | `2 * 50 - 100` → `0`; `3 < 4` → `1` |
| 3 | Constant propagation | `k = 0 … t = i + k` → `t = i + 0` |
| 4 | Copy propagation | `t1 = x … t2 = t1 + 1` → `t2 = x + 1` |
| 5 | Dead code elimination | temporaries never read again |
| 6 | Unreachable code removal | everything after a `GOTO` up to the next label |
| 7 | Branch simplification | `IF_FALSE 1 GOTO L` deleted, `IF_FALSE 0 GOTO L` → `GOTO L` |
| 8 | **Loop-safe constants** (new) | see below |
| 9 | **Jump-to-next removal** (new) | `GOTO L` or `IF_FALSE t GOTO L` immediately before `L:` |
| 10 | **Copy coalescing** (new, peephole) | `t0 = s + t1 ; s = t0` → `s = s + t1` |
| 11 | **Redundant store removal** (new) | `x = 0` where `x` is already known to be 0 |

**Why the loop-aware techniques were needed.** Every fact is thrown away at
a label, because control can arrive there from somewhere else (the back
edge). That is what keeps propagation sound around loops, but it also meant
the optimizer did almost nothing to loop bodies. On the hot-loop benchmark
the original Topic 4 optimizer saved **2 of 2,236** executed instructions, and
on the sieve it saved **none** (we measured both). Three precise rules fix it
without giving up soundness:

- **Loop-safe constants (8).** A *local scalar* assigned exactly once in
  its function, with a constant, in the *entry block* (before the first label
  or branch), holds that constant everywhere after. The entry block runs first
  and only once, nothing else writes the name, and a call cannot write it,
  since scalars are never passed by reference. Those facts are re-installed
  after every label and call. In `t4_10`, `k = 2*50-100` folds to 0, and `+ k`
  then vanishes *inside* the loop. Globals, parameters and arrays never
  qualify.
- **Sharper dead-code test (5).** The temporary allocator reuses names, so
  "is `t1` read anywhere later?" was almost always *yes*. Our TAC generator
  guarantees that **a temporary never lives across a basic-block boundary**,
  so a temporary is dead once it is overwritten or a label or branch is
  reached. `codegen.c` relies on the same invariant (below).
- **Copy coalescing (10)** removes one copy per assignment statement. In a
  loop body, that is one per iteration.

After optimization the hot loop's body is 4 TAC instructions plus the test
and the back edge (before: 7 plus 3):

```
L0:  t1 = i <= 100 ; IF_FALSE t1 GOTO L1 ; t1 = i * 2 ; s = s + t1 ; i = i + 1 ; GOTO L0
```

**Safety rules kept from Topic 3:** facts are cleared at labels; a `CALL`
drops facts about variables and about temporaries that *copy* a variable (so
`t0 = g ; CALL f ; … t0 …` is never rewritten back to the changed `g`); stores
to arrays and user variables are never removed as dead.

## Back end (`codegen.c`)

The register cache is **written back at every label and branch**, because
two paths (fall-through and the back edge) can reach a label with different
values in `$t0`–`$t9`, and memory is the only state they agree on. Under `-O2`
the back end also:

- **does not store dead temporaries** at labels and branches (the invariant
  above), and at `return` / end of function writes back **only globals**
  (locals die with the frame; restored from Project 3);
- **keeps clean registers on the fall-through path** of a conditional
  branch, since it is reachable only from the branch and nothing changed;
- **fuses compare-and-branch:** `t = i <= 100 ; IF_FALSE t GOTO L1` becomes
  `bgt $t0, 100, L1` (the inverted relation, with an immediate);
- **uses immediates and strength reduction:** `i + 1` → `addi`, `i * 2` →
  `sll … 1`.

The hot loop went from 22 MIPS instructions per iteration to 9:

```
L0: lw   $t0, 0($sp)       # i
    bgt  $t0, 100, L1      # fused: i <= 100
    sll  $t1, $t0, 1       # i * 2
    lw   $t2, 4($sp)       # s
    add  $t2, $t2, $t1     # s = s + t1
    addi $t0, $t0, 1       # i = i + 1
    sw   $t0, 0($sp)       # write back before the back edge
    sw   $t2, 4($sp)
    j    L0
```

## Benchmark: optimized vs unoptimized

`make bench` compiles each program three times, checks all three print the
expected output under SPIM, and counts what each one **executes** with
`bench/mipscount.py`. That small MIPS executor counts every instruction it runs.
It is needed because SPIM can run a program but cannot count. Its output is
checked against SPIM's for every test program. A count does not depend on
machine speed. A SPIM pseudo-instruction such as `mul` or `sle` counts once.

| Program | -O0 (none) | -O1 (TAC optimizer) | **-O2 (default)** | Original Topic 4 compiler | -O2 vs -O0 |
|---|---:|---:|---:|---:|---:|
| `t4_10_bench_hotloop` (100 iterations) | 2,236 | 1,629 | **926** | 2,234 | **2.41×** fewer |
| `t4_15_bench_sieve` (sieve to 200, nested loops, arrays) | 14,555 | 12,867 | **8,660** | 14,555 | **1.68×** |
| `t4_16_bench_nested` (2- and 3-deep `for` nests) | 3,568 | 2,794 | **1,725** | 3,548 | **2.07×** |
| `t4_06_optimize` (straight-line) | 31 | 22 | **19** | 22 | 1.63× |

Memory traffic (loads + stores) on the hot loop: 910 → 407. Static size: 24 → 18
TAC instructions, 53 → 33 MIPS instructions. Wall-clock SPIM time (in the
`make bench` table) is dominated by start-up at this size, which is why
executed instructions are the headline metric.

## Test programs (44 — 14 are supposed to fail)

| File | Shows | Expected |
|---|---|---|
| `t2_01` – `t2_11` | the Topic 2 suite (file-scope statements → implicit `main`) | as in Project 2 |
| `t3_01` – `t3_13` | the Topic 3 suite: precedence, arrays, functions, scope, fact(5), by-reference arrays, nested blocks, spills; 4 failing programs | as in Project 3 |
| `t4_01_while` | `while`, sum 1..10 | `55 10` |
| `t4_02_for` | `for` | `30 120` |
| `t4_03_break` | `break` in `while` and `for` (update skipped) | `21 4` |
| `t4_04_nested` | nested loops | `225 9` |
| `t4_05_relational` | all six relational operators yield 1/0 | `1 0 1 0 1 0` |
| `t4_06_optimize` | constant folding, algebraic simplification, propagation | `10` |
| `t4_07_zero_iter` | test at the top: a false condition runs the body 0 times | `0 7` |
| `t4_08_precedence` | `a + 1 < b * 2` parses as `(a + 1) < (b * 2)` | `1 0 7` |
| `t4_09_nested_break` | `break` leaves only the innermost loop | `33 3` |
| `t4_10_bench_hotloop` | benchmark 1: a hot loop with redundant arithmetic | `10100` |
| `t4_11_loop_scope` | a variable declared in a loop body is a new variable (`i__1`) | `0 150 3 7` |
| `t4_12_loops_functions` | loops in functions, a call in a loop condition, an array filled by one loop and summed by another (by reference), fib(20) | `55 6765 10 40 120 9` |
| `t4_13_toplevel_loops` | a Topic 2 style program with loops (implicit `main`) | `30 16 5` |
| `t4_14_for_variants` | `for(;;)`, `for(; c ;)`, no condition, `break` skips the update, an unbraced body | `5 10 1 0 8` |
| `t4_15_bench_sieve` | benchmark 2: Sieve of Eratosthenes | `46 199` |
| `t4_16_bench_nested` | benchmark 3: nested `for` loops | `2200 21` |
| `tests_neg/t4_neg_break_outside_loop` | `break` outside any loop | semantic error, line 5 |
| `tests_neg/t4_neg_break_missing_semicolon` | `break` with no `;` | "missing ';' after 'break'", line 9 |
| `tests_neg/t4_neg_loop_semantics` | `break` in a function called from a loop, an array as a loop condition, undeclared loop bound, a whole array in a for-init, a loop-body variable used after the loop | 5 errors, each with its line |
| `tests_neg/t4_neg_toplevel_and_main` | a file-scope loop in a program that also has `main()` | 1 error |

`make check` runs every passing program at **all three** optimization levels,
so an optimization that changed a program's meaning would fail the suite.
`make fuzz` goes further. It generates random programs with loops nested 3
deep, `break`, a global changed by a function called inside expressions,
arrays and shadowing blocks. It compiles each at all three levels and
compares the result with a reference interpreter in Python. 600 programs × 3
levels: 0 failures. The fuzzer found two real bugs that were already in the
Topic 4 starting code at every level, and both are fixed:
1. **Register eviction.** Under register pressure an instruction could evict
   its own operand (`arr[1] = 5` stored an address instead of 5). Register
   pinning is restored.
2. **Evaluation order.** Operand order was not left to right (see the next
   section).

## Language design choices

**1. Relational operators sit below arithmetic, on one shared level, left
associative.**

```c
%left '<' '>' LE GE EQ NE      /* lowest  */
%left '+' '-'
%left '*' '/'
%right UMINUS                  /* highest */
```

So `a + 1 < b * 2` means `(a + 1) < (b * 2)` (`t4_08`), which is what every
reader expects. C uses two levels (`< > <= >=` above `== !=`). We use one,
because without `&&` and `||` (Topic 5) the only visible difference is in
chains like `a < b == c`. One level is simpler to state and to teach, and it
still gives a defined result: left to right, `(a < b) == c`. Unary minus binds
tightest, so `-3 < -2` is `1`.

**2. A comparison is an `int` that is 1 or 0.** There is no separate boolean
type, as in C, so `print(3 < 4)` prints `1` and `count = count + (x == y)`
works. A loop condition is "true" when non-zero, so `while (1)` is the
infinite loop. On MIPS this maps directly onto `slt`/`sle`/`seq`…, which
write exactly 1 or 0.

**3. Two loops: `while`, and a C-style `for`. No `do-while`.** These two
cover test-at-top iteration, and `for` packages the counting pattern. All
three `for` header parts are optional: an empty condition means "always true",
so `for (;;)` is an infinite loop that only `break` can leave (`t4_14`). The
header's init and update are restricted to assignments (`for (i = 0; …; i = i + 1)`)
and cannot declare a variable. That keeps the grammar LALR(1) with zero
conflicts and avoids a scope question that C99 needed a special rule for.

**4. The test is at the top.** Both loops test before the first iteration,
so `while (i < 3)` with `i = 5` runs zero times (`t4_07`). A test-at-bottom
lowering saves one jump per iteration, but it would run the body once when
it should not.

**5. `for` is lowered directly, not rewritten into `while`.** The update has
a fixed place, right before the back edge. `break` correctly skips it
(`t4_14`: `i` stays 0). A future `continue` can jump to the update, which a
naive "`init; while (c) { B; upd; }`" rewrite gets wrong.

**6. `break` leaves the innermost loop, is lexical, and is checked
semantically.** A grammar cannot count enclosing loops, so `break;` parses
anywhere and `semantic.c` keeps a loop depth. Only the loop *body* counts.
Being called from a loop does not, so `break` inside `helper()` is an error
even when `helper()` is called inside a `while` (`t4_neg_loop_semantics`,
line 14). In TAC, a break-label stack gives the target. The language has no
`if` yet, so `while (c) { …; break; }` serves as "if c" (used in `t4_15`).

**7. Loop bodies are scopes.** A body in `{ }` is a block, so a variable
declared inside it is a new variable that disappears at the `}`. Using it
after the loop is an error. Assigning it never touches an outer variable of
the same name: `tac.c` renames it `i__1` with its own frame slot (`t4_11`).
Names containing `__` are reserved, so a user name can never collide with a
renamed one.

**8. Strictly left-to-right evaluation.** C leaves operand order
unspecified. We define it: in `g + f(0)`, `g` is read *before* `f` runs, even
if `f` changes `g`. The same holds for call arguments and for `a[i] = f()`,
where the index comes first. A defined order means a program has one meaning
on every compiler level. The fuzzer depends on it, and it found that the
starter read `g` after the call. It is implemented by snapshotting the earlier
variable into a temporary only when a call appears to its right, so ordinary
expressions pay nothing.

**9. Topic 2 programs still compile.** Statements, including loops, at file
scope form an implicit `main()` (`t4_13`). Defining `main()` as well is an
error, because those statements could never run (`t4_neg_toplevel_and_main`).

**10. Errors are reported, not crashed on.** Every error carries a line:
"missing `;` after 'break'" names the line of the `break`, not the `}` where
the parser noticed. Semantic analysis reports *all* errors in a program, not
just the first (`t4_neg_loop_semantics` reports 5).

**Known limitations.** Loops can nest at most 32 deep (`MAX_LOOP_DEPTH`; a
deeper nest is a reported error, not a crash). There is no `continue` yet.
There is no bounds checking on arrays, as in C. The code generator keeps
values in registers only within a basic block: every label and back edge
goes through memory, so loop variables are reloaded each iteration. Keeping
them in registers across the loop needs liveness analysis, the natural next
step.

## File map

```
compiler/
  scanner.l  parser.y          phases 1–2
  ast.h ast.c                  AST nodes, constructors, printAST
  semantic.h semantic.c        phase 3
  symtab.h symtab.c            storage map used by the back end
  tac.h tac.c                  phase 4 (TAC) and phase 5 (optimizer)
  codegen.h codegen.c          phase 6 (MIPS)
  main.c                       driver, -q / -O0 / -O1, timing, scorecard
  tests/  tests_neg/           44 programs (see table)
  run_tests.sh                 make check
  bench/mipscount.py           executed-instruction counter (make bench)
  bench/run_bench.sh           make bench
  bench/fuzz_gen.py run_fuzz.sh  make fuzz
```
