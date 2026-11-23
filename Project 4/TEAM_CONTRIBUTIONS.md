# CST-405 Project 4 — Team Contributions

**Course:** CST-405 Principles of Compiler Design · Grand Canyon University
**Assignment:** Project 4 — Updated Compiler to Support Loops (Topic 4)
**Team:** Christoffer Hansen, Emma Schaffner

## Loom links

| Team member | Video |
|---|---|
| Christoffer Hansen | https://www.loom.com/share/2b8a673a89164fe08a630d245656df15 |
| Emma Schaffner | https://www.loom.com/share/018c201e5206453096af8ef829db4016 |

## Who did what

| Team member | Focus | Files |
|---|---|---|
| **Emma Schaffner** | The loop language feature, end to end: tokens → grammar → AST → semantic check → TAC lowering → MIPS branches | `scanner.l`, `parser.y`, `ast.h`, `ast.c`, `semantic.c`, `tac.c` (loop lowering, branch simplification), `codegen.c` (labels and branches), `tests/t4_01`–`t4_10`, `tests_neg/t4_neg_break_outside_loop` |
| **Christoffer Hansen** | Carrying Topics 2–3 forward, the loop-aware optimizer and back end, measurement and verification, documentation | `tac.c` (scope renaming, implicit `main`, evaluation order, optimizer techniques 8–11), `codegen.c` (pinning, peepholes), `semantic.c` (restored checks), `parser.y` / `scanner.l` (restored error handling), `main.c`, `Makefile`, `run_tests.sh`, `bench/`, `tests/t2_*`, `t3_06`–`t3_13`, `t4_11`–`t4_16`, three new `tests_neg` programs, README, this file |

### Emma Schaffner

- **Phase 1, lexical analysis (`scanner.l`):** the `while`, `for` and
  `break` keywords, placed above the identifier rule so first-match makes
  them keywords. `<= >= == !=` are their own rules, so longest-match keeps
  each one a single token, and `<` `>` are returned as characters.
- **Phase 2, grammar (`parser.y`):** relational operators added to the
  precedence table *below* `+ -`, so `a + 1 < b * 2` groups correctly.
  Also `while (cond) stmt`, `for (init; cond; update) stmt` with every header
  part optional (`for_init`, `for_cond`, `for_update`), and `break;`.
  Bison reports 0 conflicts.
- **Phase 2, AST (`ast.h`, `ast.c`):** `NODE_WHILE`, `NODE_FOR`, `NODE_BREAK`,
  their constructors, the packed relational-operator codes with `opText`, and
  `printAST` cases. A `NULL` for-condition means "always true".
- **Phase 3, semantic analysis (`semantic.c`):** the `break` depth counter
  (only the loop *body* is inside the loop) and the "break outside of any
  loop" error with its line. `while` and `for` checks cover the condition,
  init and update.
- **Phase 4, intermediate code (`tac.c`):** `newLabel`. Loops are lowered
  test-at-the-top with `LABEL` / `IF_FALSE` / `GOTO`. `for` is lowered
  directly, not rewritten into a `while`, so the update has a fixed place
  before the back edge. A break-label stack gives `break` the innermost
  loop's exit.
- **Phase 5, optimizer (`tac.c`):** branch simplification. A constant
  condition becomes a `GOTO` or nothing, and the next pass removes the code
  that `GOTO` makes unreachable, which shows why the optimizer runs to a
  fixed point.
- **Phase 6, MIPS (`codegen.c`):** label and branch emission (`j`, `beq`,
  `bne`) and `slt sgt sle sge seq sne` for the relational operators. The
  register cache is written back at every label and branch.
- **Tests:** `t4_01`–`t4_10` (while, for, break, nesting, relational
  operators, optimization, zero iterations, precedence, nested break, the
  hot-loop benchmark) and `tests_neg/t4_neg_break_outside_loop`.

### Christoffer Hansen

- **Bringing Topics 2 and 3 forward (the "Updates" criterion).** The Topic 4
  starting code had lost much of our Project 3 work, and the Topic 2 tests no
  longer compiled. Restored and re-tested:
  - **Implicit `main`** for file-scope statements (`parser.y` `top_stmt`,
    `semantic.c`, `tac.c`), now including loops, so all 11 Topic 2 programs
    compile again. Added the entry-point rule (exactly one of `main()` or
    file-scope statements; `main` takes no parameters).
  - **Scope renaming** in `tac.c` (`x__1`). Without it a variable declared
    inside a loop body overwrote the outer variable of the same name, and
    `t3_08_blocks` printed wrong values.
  - **Error handling:** lexical errors fail the compile and name their line,
    `\r` is whitespace, syntax errors are deferred and reported once, and the
    "missing `;`" error productions are back, plus a new one for `break`.
  - **Semantic checks:** duplicate and reserved parameters, indexing a
    scalar, an array used as a value (which also catches `while (a)`),
    assigning a whole array, a scalar passed for `int[]`, at most 4
    parameters, params and body sharing one scope, duplicate functions
    reported with both lines. Also fixed a new bug: in calls with 3+
    arguments the first arguments were never checked, and an undeclared one
    crashed the code generator.
  - **Register pinning** in `codegen.c`, so an instruction never evicts its
    own operands. `return` writes back only globals.
- **Loop-aware optimizer (`tac.c`).** I measured that the starting optimizer
  saved 2 of 2,236 executed instructions on the hot-loop benchmark and none
  on the sieve, because every fact is discarded at a label. Added:
  - (8) **loop-safe constants**, propagated across labels when that is
    provably sound
  - a sharper **dead-code** test, based on the invariant that temporaries
    never cross a basic-block boundary
  - (9) **jump-to-next** removal, for both `GOTO` and conditional branches
  - (10) **copy coalescing**
  - (11) **redundant store** removal
  - all of them on the scorecard

  After optimization the hot loop's body is 4 TAC instructions plus the
  test and the back edge (before: 7 plus 3).
- **Back-end peepholes (`codegen.c`):**
  - no stores of dead temporaries at labels and branches
  - clean registers kept on a branch's fall-through path
  - compare-and-branch fusion (`bgt $t0, 100, L1`)
  - `addi` immediates and `sll` strength reduction

  The hot loop went from 22 to 9 MIPS instructions per iteration.
- **Evaluation order.** Defined it as strictly left to right and
  implemented it. The starter read `g` *after* the call in `g + f(0)`. The
  optimizer now forgets temporaries that copy a variable at every `CALL`,
  so the ordering is kept.
- **Measurement:**
  - `-O0` / `-O1` options in `main.c`
  - `bench/mipscount.py`, an executed-instruction counter validated against
    SPIM on every test program
  - `make bench`, the optimized vs unoptimized table: up to **2.41× fewer
    instructions executed**
- **Verification:**
  - `run_tests.sh` / `make check`: 44 programs, each passing one checked at
    `-O0`, `-O1` and `-O2`. **44 passed, 0 failed.**
  - `make fuzz`: random loop programs vs a Python reference interpreter,
    600 × 3 levels with 0 failures. It found the register-eviction and
    evaluation-order bugs above.
  - a clean `-Wall` build
- **Tests:**
  - brought the 11 Topic 2 tests and `t3_06`–`t3_13` forward
  - added `t4_11` (loop scope), `t4_12` (loops with functions and arrays),
    `t4_13` (file-scope loops), `t4_14` (`for` variants), and the
    `t4_15` / `t4_16` benchmarks
  - added three negative programs (break missing `;`, five loop semantic
    errors, file-scope loop plus `main`)
  - corrected the line number in the header of `t4_neg_break_outside_loop`
- **Documentation:** README (checklist, phase table, loop lowering,
  optimizer, back end, benchmark, test table, language design choices) and
  this file.

## Our approach to integrating increasingly complex components

1. **Re-baseline first.** Before adding anything, Christoffer ran the
   Project 3 suite against the Topic 4 starting code. Six programs failed,
   which showed which Project 3 features had been lost, and those were
   restored first. New loop code should never be debugged on top of a broken
   base.
2. **One loop, through every phase, before the next.** Emma took `while`
   from token to MIPS branch (scanner → grammar → AST → semantic check →
   TAC labels → `beq`/`j`). `for` and `break` then reused the same
   label/jump machinery: `for` adds an init and an update, and `break` adds
   one stack.
3. **An agreed interface between front and back end.** The loop nodes' shape
   (`NODE_WHILE(cond, body)`, `NODE_FOR(init, cond, update, body)`, a `NULL`
   condition meaning "true") and the TAC pattern were fixed up front. That let
   the lowering and the optimizer/back-end work proceed in parallel.
4. **Correctness before speed, and speed measured.** Every optimization was
   added behind the `-O0` / `-O1` switch. The suite checks every program at
   all three levels, and `make bench` measures what each level buys. A change
   that made the code faster but changed a program's output fails
   `make check` immediately.
5. **The full suite after every merge** (`make check`, plus `make fuzz` for
   optimizer changes), so each new feature was shown not to break the earlier
   ones, Topics 2 and 3 included.

## Video checklist (each member records their own)

a) **How the code works.** Walk the six phases on a loop program:
   `./minicompiler tests/t4_10_bench_hotloop.cm out.s` (AST → semantic trace →
   TAC with labels → scorecard and fixed point → MIPS).
   - Emma: tokens, the precedence table, the loop grammar and AST, the
     `break` depth check, how `while`/`for`/`break` become labels and jumps,
     and branch emission.
   - Christoffer: what was restored from Project 3 (`t3_08`, `t2_*`),
     loop-safe constants and the other new techniques (`diff out.tac
     out.optimized.tac`), the back-end peepholes (the 9-instruction loop),
     and evaluation order.

b) **Your individually assigned tasks.** Your section above.

c) **The team's integration approach.** The five steps above.

d) **Execution.**
   1. `make check` (44 passed, 0 failed).
   2. `spim -file out.s` on `t4_10` (10100), then `make bench`
      (optimized vs unoptimized).
   3. One failing program, e.g. `./minicompiler tests_neg/t4_neg_loop_semantics.cm out.s`
      (5 errors, each with its line).
