# CST-405 · Topic 2 — Compiler for a Starter Language

**Weeks 2–5 · Sep 14 – Oct 11, 2026**

A whole compiler, end to end, for the smallest language worth compiling.

---

## What you are given

This folder is a **working compiler**. Build it before you change anything:

```bash
cd compiler
make
```

- `scanner.l` — a reference version of the Project 1 scanner, adapted to return bison token numbers
- every `.h` file — the data structures are the design
- `parser.y` prologue: `%union`, `%token`, the precedence table, `yyerror`
- `tac.c` machinery: temporary and label allocation, list handling
- `codegen.c` parts 1–3: the register cache, addressing, frame layout
- `main.c` — the six-phase driver

## What you are building

- the grammar rules and their semantic actions (`parser.y`)
- the AST constructors and `printAST` (`ast.c`)
- symbol table insert and lookup (`symtab.c`)
- the semantic checks (`semantic.c`)
- AST → TAC (`tac.c`)
- one optimization pass (`tac.c`)
- TAC → MIPS instruction selection (`codegen.c`)

Every place you need to write something is marked `TODO (Topic 2)` in the source,
with the guidance you need at that spot. Work through them in the order they appear
in the file — they are ordered deliberately.

```bash
grep -rn "TODO (Topic 2)" compiler
```

## Building and testing

```bash
cd compiler
make                                   # build ./minicompiler
make test                              # compile and run every tests/*.cm
make clean                             # remove everything make produced

./minicompiler tests/t2_05_errors_syntax.cm out.s          # full trace of all six phases
./minicompiler tests/t2_05_errors_syntax.cm out.s -q       # quiet: errors and summary only
spim -file out.s                       # run the generated MIPS
```

Compiling also writes `out.tac` and `out.optimized.tac`. Reading those two side by
side is the fastest way to see what the optimizer did.

## You are done when

- `make test` runs every program in `tests/`
- `t2_01`–`t2_03` produce the outputs in their header comments
- `t2_04` and `t2_05` are supposed to FAIL — check the messages name the right line

## The rest of this topic

- **Lecture notes** — `../../docs/topic-2-minimal-compiler/lecture-notes.html`
- **Class activities** — `../../docs/topic-2-minimal-compiler/` (one per class meeting)
- **The assignment** — `../../docs/topic-2-minimal-compiler/CST-405-Topic-2-Project-2.docx`
- **Grammar reference** — `../../docs/C-Minus-Grammar-Reference.md`

## Requirements

`flex`, `bison`, `gcc`, `make`, and [SPIM](http://spimsimulator.sourceforge.net/)
or QtSPIM to run the generated assembly.


---

## Running on Ubuntu (step by step)

```bash
# 1. Install the toolchain (once)
sudo apt update
sudo apt install -y build-essential flex bison spim
#    optional GUI simulator:  sudo apt install -y qtspim   (or download QtSpim)

# 2. Build
cd "Project 2/compiler"
make clean && make            # produces ./minicompiler, no warnings

# 3. Run the whole regression suite (compiles every tests/*.cm and runs it in SPIM)
make test

# 4. Demo a single program with the full six-phase trace
./minicompiler tests/t2_01_basics.cm out.s
cat out.tac                   # unoptimized 3-address code (IR)
cat out.optimized.tac         # IR after optimization
cat out.s                     # MIPS assembly (after peephole)
spim -file out.s              # run it -> 5 10 18
```

On Windows, run the same commands inside WSL (Ubuntu): the repo is visible at
`/mnt/c/Users/<you>/Documents/GitHub/CST-405/Project 2`.

## Test programs (11 — 6 are supposed to fail, all exit with code 1)

| File | Shows | Expected |
|---|---|---|
| `t2_01_basics.cm` | declare, assign, add, print | `5 10 18` |
| `t2_02_chained.cm` | left-associative `+` in the AST | `6 21` |
| `t2_03_comments.cm` | line + block comments | `42` |
| `t2_04_errors_undeclared.cm` | **semantic** error: undeclared `ghost` | fails, line 8 |
| `t2_05_errors_syntax.cm` | **syntax** error: missing `;` after assignment | fails, line 7 |
| `t2_06_optimizations.cm` | algebraic simplification, folding, propagation, DCE, peephole | `7 7 14 0` |
| `t2_07_errors_duplicate.cm` | **semantic**: duplicate declaration + undeclared, both reported | fails, lines 7 and 9 |
| `t2_08_errors_lexical.cm` | **lexical** error: unknown character `@` | fails, line 9 |
| `t2_09_errors_decl_semicolon.cm` | **syntax**: missing `;` after declaration | fails, line 6 |
| `t2_10_errors_print_semicolon.cm` | **syntax**: missing `;` after print | fails, line 7 |
| `t2_11_longer_program.cm` | longer end-to-end demo program | `3 6 12 24 48 99` |

Every test states its expected result in its header comment.

## Inspecting each phase individually

A full (non `-q`) run prints every stage in order:

| Stage | Where it appears |
|---|---|
| Token stream | Phase 1 — `line / token / lexeme` for every token |
| AST | Phase 2 — indented tree from `printAST()` |
| Symbol table | Phase 3 (semantic scope table) and Phase 6 (name → stack offset) |
| Unoptimized TAC | Phase 4, and the file `<out>.tac` |
| Optimized TAC | Phase 5 with a per-technique count and instructions removed, and `<out>.optimized.tac` |
| MIPS | the `.s` file; Phase 6 also lists every peephole rewrite |

`-q` silences all of it and prints only errors. The compiler exits **0** on success and **1** on any lexical, syntax or semantic error.

## Optional extensions claimed

- **Dead code elimination** (TAC): only assignments to compiler temporaries (`t0`, `t1`, ...) that are never read again are removed. Stores to user variables are always kept, because a later topic's control flow could read them.
- **Peephole optimization** on the generated MIPS (`codegen.c`, part 5).
- **Algebraic simplification** and **flow optimization** in the TAC optimizer.
- **Clearer syntax errors**: bison's verbose messages ("unexpected ';', expecting NUM or ID") plus error productions for a missing `;` after each statement form, using `%locations` so the line named is the one that is actually wrong.

## CST-405 Compiler Design Checklist — where each item lives

| Checklist item | Where |
|---|---|
| Lexical analyzer | `scanner.l` (flex): token stream displayed, unknown characters reported with line and fail the compile |
| Symbol table | `symtab.c/.h` (insert/lookup, frame offsets; printed in Phase 6) + scope table in `semantic.c` (printed in Phase 3) |
| Parse tree creation + display | grammar actions in `parser.y` build the tree; `printAST()` in `ast.c` displays it (Phase 2) |
| Verifies tokens follow the grammar | `parser.y` (bison LALR(1)) |
| AST in a separate file | `ast.c/.h` |
| Syntax errors with location | `yyerror` + error productions in `parser.y` (`%define parse.error verbose`) |
| Certifies syntactically correct code | `✓ Parse succeeded` in `main.c` |
| Semantic checks in a separate file | `semantic.c/.h` — undeclared use, assignment to undeclared, duplicate declaration, reserved names |
| Semantic errors with location | boxed messages with `Line N` in `semantic.c` |
| Certifies semantically correct code | `✓ Semantic analysis passed` |
| 3-address IR generation | `generateTAC()` in `tac.c` |
| IR saved to a file | `<out>.tac` and `<out>.optimized.tac` |
| Flow optimization | `flowOptimize()` in `tac.c` (constant branches, jump-to-next, unreachable code) + jump-to-next removal in the MIPS peephole |
| Peephole optimization | `peepholeOptimizeFile()` in `codegen.c` (2-instruction window over the MIPS) |
| Other optimizations | `tac.c`: constant folding, constant/copy propagation, algebraic simplification, dead-code elimination, repeated to a fixed point |
| Code generator (MIPS, SPIM/QtSPIM) in a separate file | `codegen.c/.h` |
| Assembly saved to a file | the `.s` file named on the command line |

Note: the starter language has no `if`/`while`, so the TAC-level flow passes
usually report 0 — the check runs on every compile and fires once control flow
exists. The MIPS peephole's jump-to-next removal fires on every program.
