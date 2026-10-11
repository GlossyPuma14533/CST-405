# CST-405 Project 2 — Team Contributions

**Course:** CST-405 Principles of Compiler Design · Grand Canyon University
**Assignment:** Project 2 — Minimalist Language Compiler (Topic 2)
**Due:** Saturday, October 11, 2026
**Team:** Christoffer Hansen, Emma Schaffner

## Loom Links

Christoffer
https://www.loom.com/share/f5208c39439a4dbab170a18d4747667a

Emma
Link
https://www.loom.com/share/a5e27ef665af4f92abe74b0c3182f5da

## Who did what

| Team member | Phases owned | Files |
|---|---|---|---|
| **Emma Schaffner** | Phases 2–6 core implementation (the `TODO (Topic 2)` sections) | `parser.y`, `ast.c`, `symtab.c`, `semantic.c`, `tac.c`, `codegen.c` |
| **Christoffer Hansen** | Optimization extensions, peephole pass, error reporting, Phase 1 display, test suite, documentation | `codegen.c`, `tac.c`, `parser.y`, `scanner.l`, `main.c`, `semantic.c`, `tests/`, README |

### Emma Schaffner

- **Phase 2 — Syntax analysis:** the grammar rules and semantic actions in
  `parser.y` (program, statement list, declaration, assignment, expression,
  print), plus the original error productions for a missing `;`.
- **Phase 2 — AST:** the remaining AST constructors and the indented tree
  printer `printAST()` in `ast.c`.
- **Phase 3 — Symbol table:** declare (insert) and resolve (lookup) in `symtab.c`.
- **Phase 3 — Semantic analysis:** expression and statement checks in
  `semantic.c`, covering undeclared variables, assignment to undeclared
  variables, duplicate declarations and reserved names (`t0`, `L1`, ...), each
  reported with a line number.
- **Phase 4 — Intermediate code:** AST → three-address code for expressions
  and statements in `tac.c`.
- **Phase 5 — Optimization:** the required optimization pass in `tac.c`
  (constant folding, constant propagation, dead-code elimination of
  temporaries, run to a fixed point).
- **Phase 6 — Code generation:** TAC → MIPS instruction selection in `codegen.c`.

### Christoffer Hansen

- **Peephole optimization** on the generated MIPS (`codegen.c`, part 5): a
  two-instruction window that removes self-moves, jumps to the next
  instruction and redundant loads, and merges `li` + `move` pairs.
- **Flow optimization** (`flowOptimize()` in `tac.c`): constant-branch
  simplification, jump-to-next removal and unreachable-code removal.
- **Algebraic simplification and copy propagation** in the TAC optimizer
  (`x + 0`, `x * 1`, `x * 0`, ...).
- **Error reporting:**
  - lexical errors now print their line number and fail the compile;
  - deferred `yyerror` with `%define parse.error verbose`, so each error
    gives one precise message;
  - `%locations`, so a missing `;` after a declaration, assignment or print
    is reported on the correct line.
- **Token stream display** for Phase 1 (`scanner.l`).
- **Symbol table** printed after semantic analysis as well as before.
- **Tests `t2_06`–`t2_11`:** optimizations, multiple semantic errors, a
  lexical error, missing `;` after a declaration and after a print, and a
  longer end-to-end program.
- **Documentation:** RECEIVES/PRODUCES headers on every source file,
  known-limitation comments, `.gitignore`, the README (Ubuntu build steps,
  checklist mapping, test table, optional extensions) and this file.
- **Verification:** clean build with no warnings, and all 11 tests passing.

## Repository

Private GitHub repository: https://github.com/GlossyPuma14533/CST-405.git
