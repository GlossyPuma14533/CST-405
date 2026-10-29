# CST-405 · Topic 3 — Compiling Complex Variables and Functions

**Weeks 6–8 · Oct 12 – Nov 1, 2026**

Arrays, functions, scope — and the activation record that makes calls possible.

---

## What you are given

This folder is a **working compiler**. Build it before you change anything:

```bash
cd compiler
make
```

- the complete, working Topic 2 compiler — it builds and passes the Topic 2 tests before anything is written

## What you are building

- arithmetic: `- * /`, parentheses, unary minus, and the precedence table
- arrays: declaration, indexing, array parameters
- functions: definitions, parameters, calls, `return`
- scope: a scope stack, and two-pass semantic analysis
- activation records: prologue, epilogue, saved `$ra`, arguments in `$a0`–`$a3`

Every place you need to write something is marked `TODO (Topic 3)` in the source,
with the guidance you need at that spot. Work through them in the order they appear
in the file — they are ordered deliberately.

```bash
grep -rn "TODO (Topic 3)" compiler
```

## Building and testing

```bash
cd compiler
make                                   # build ./minicompiler
make test                              # compile and run every tests/*.cm
make clean                             # remove everything make produced

./minicompiler tests/t3_05_scope.cm out.s          # full trace of all six phases
./minicompiler tests/t3_05_scope.cm out.s -q       # quiet: errors and summary only
spim -file out.s                       # run the generated MIPS
```

Compiling also writes `out.tac` and `out.optimized.tac`. Reading those two side by
side is the fastest way to see what the optimizer did.

## You are done when

- all five `t3_*` tests produce the outputs in their header comments
- the Topic 2 tests still pass
- `fact(5)` returns 120 — if it does not, the calling convention is wrong

## The rest of this topic

- **Lecture notes** — `../../docs/topic-3-arrays-and-functions/lecture-notes.html`
- **Class activities** — `../../docs/topic-3-arrays-and-functions/` (one per class meeting)
- **The assignment** — `../../docs/topic-3-arrays-and-functions/CST-405-Topic-3-Project-3.docx`
- **Grammar reference** — `../../docs/C-Minus-Grammar-Reference.md`

## Requirements

`flex`, `bison`, `gcc`, `make`, and [SPIM](http://spimsimulator.sourceforge.net/)
or QtSPIM to run the generated assembly.


---

# Our implementation (Project 3)

Every `TODO (Topic 3)` in the starter has been implemented and is now marked
`DONE (Topic 3)` with a comment explaining the design decision at that spot.

```bash
cd compiler
make clean && make        # builds with -Wall and no warnings
make check                # 24 tests, PASS/FAIL against each header — 24 passed
```

## Running on Ubuntu / WSL (step by step)

```bash
sudo apt update && sudo apt install -y build-essential flex bison spim
cd "Project 3/compiler"
make clean && make
make check                                    # automatic regression suite
./minicompiler tests/t3_06_fact.cm out.s      # full six-phase trace
cat out.tac out.optimized.tac                 # IR before / after optimization
spim -file out.s                              # -> 120 120 24
```

## What was added, phase by phase

| Phase | File | Topic 3 work |
|---|---|---|
| 1 Lexical | `scanner.l` | `return`; `- * /`; `{ } [ ] ,`; `\r` whitespace; lexical errors counted and fatal; `yylloc` for error lines |
| 2 Syntax | `parser.y` | program = globals + functions; `int a[10]`, `a[i]`, `int a[]`; `- * /`, `( )`, unary minus via `%prec UMINUS`; definitions, params, calls, `return`, blocks. **0 conflicts.** Precise "missing `;`" errors carried over from Project 2 |
| 2 AST | `ast.h`, `ast.c` | 9 new node kinds, constructors, `printAST` for each |
| 3 Semantic | `semantic.c` | scope **stack**; function table seeded with `print/1`; **two passes** (signatures, then bodies) so calls may precede definitions; arity, array-vs-scalar argument checks, array size > 0, return outside function, exactly one `main`, max 4 params |
| 4 TAC | `tac.c` | `FUNC_BEGIN/END`, `PARAM`, `ARG`, `CALL`, `RETURN`, `ARRAY_DECL/LOAD/STORE`; block-scoped locals renamed (`x__1`) so each gets its own frame slot |
| 5 Optimizer | `tac.c` | unchanged passes, plus a fix: facts that are copies of variables are dropped at a `CALL` |
| 6 MIPS | `codegen.c` | activation records (prologue/epilogue, saved `$ra`), params `$a0–$a3` → frame, args loaded **before** the caller-saved flush, `$v0` return, `return` jumps to the epilogue; array addressing for global / local / by-reference arrays; register pinning so one instruction never evicts its own operands |

## Activation record

```
 high   ┌──────────────────────┐
        │ saved $ra            │ frameSize-4($sp)
        │ (alignment)          │ frameSize-8($sp)
        │ params, locals,      │
        │ arrays, temporaries  │ 0($sp) ...
 low    └──────────────────────┘ <- $sp
```

Caller: evaluate args → `$a0–$a3` (arrays as base address) → write back dirty
`$t` registers → `jal` → result from `$v0`.
Callee: `addi $sp,-N` → `sw $ra` → copy `$aK` into the parameter slots →
body → `return` puts the value in `$v0` and jumps to `f__epilogue` →
`lw $ra` → `addi $sp,+N` → `jr $ra`.

## Test programs (24 — 11 are supposed to fail)

| File | Shows | Expected |
|---|---|---|
| `t2_01` – `t2_11` | the Topic 2 suite, unchanged (file-scope statements become an implicit `main`) | as in Project 2 |
| `t3_01_arithmetic` | precedence, associativity, parentheses, unary minus | `14 20 9 3 -20 7` |
| `t3_02_globals` | globals in `.data`, a call that reads one | `5 12 17` |
| `t3_03_arrays` | global and local arrays, array parameter | `30 100 60` |
| `t3_04_functions` | 3 params, nested calls on either side | `6 5 7 8` |
| `t3_05_scope` | parameter shadows a global | `1 2 1` |
| `t3_06_fact` | **fact(5) = 120** through a 6-deep call chain | `120 120 24` |
| `t3_07_by_reference` | callee writes caller's array, computed indices, forwarding an array param | `0 10 20 30 30 25 13 7` |
| `t3_08_blocks` | the same name at four depths, each its own slot | `1 2 3 2 1 100 42` |
| `t3_09_expressions` | arithmetic on run-time values; an expression that forces register spills | `21 -5 2 12 -7 -22` |
| `t3_10_errors_calls` | wrong arity, undefined function, scalar for array param, duplicate function | 4 errors |
| `t3_11_errors_arrays` | size 0, indexing a scalar, array as value, assigning a whole array | 4 errors |
| `t3_12_errors_scope` | use after block ends, duplicate in block, param redeclared, `return` at file scope | 5 errors |
| `t3_13_errors_too_many_params` | 5 parameters (only `$a0–$a3` exist) | 1 error |

## Design decisions and known limitations

- **No `if` yet**, so a recursion that terminates cannot be written until
  Topic 4. `t3_06` proves the calling convention with a real chain of six
  frames instead; the same prologue/epilogue code is what recursion will use.
- **No array bounds checking** — at compile time it is undecidable in general,
  and like C there is no run-time check. A deliberate choice, not an oversight.
- **At most 4 parameters**, because arguments travel only in `$a0–$a3`;
  more is a semantic error rather than silently wrong code.
- **An array name is not an expression** (`createArrayIndex` stores a name),
  which keeps pointers out of the language.
- **Two-pass analysis**: a function may call one defined later in the file;
  a function may only use globals declared above it (as in C).
- **Topic 2 compatibility**: a program with statements at file scope and no
  `main` gets an implicit `main`; mixing that with an explicit `main` is an error.
