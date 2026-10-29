# CST-405 Project 3 — Team Contributions

**Course:** CST-405 Principles of Compiler Design · Grand Canyon University
**Assignment:** Project 3 — Compiling Complex Variables and Functions (Topic 3)
**Due:** Sunday, November 1, 2026
**Team:** Christoffer Hansen, Emma Schaffner

## Loom links

| Team member | Video |
|---|---|
| Christoffer Hansen | _<link>_ |
| Emma Schaffner | _<link>_ |

## Who did what

| Team member | Phases owned | Files |
|---|---|---|
| **Emma Schaffner** | Front end and semantic analysis: phases 1–3 | `scanner.l`, `parser.y`, `ast.h`, `ast.c`, `semantic.c` |
| **Christoffer Hansen** | Back end and verification: phases 4–6, tests, documentation | `tac.c`, `codegen.c`, `main.c`, `tests/`, `run_tests.sh`, `Makefile`, README |

### Emma Schaffner

- **Phase 1 — Lexical analysis (`scanner.l`):** the `return` keyword; the
  `- * /` operators (checking that `/` never swallows the start of a comment);
  the `{ } [ ] ,` tokens; `\r` handling for files saved on Windows.
- **Phase 2 — Grammar (`parser.y`):** a program is now a list of global
  declarations and function definitions. Added:
  - array declarations `int a[10];`, indexing `a[i]`, and array
    parameters `int a[]`
  - assignment to array elements
  - function definitions, parameter lists, calls and `return`
  - `{ }` blocks
  - full arithmetic using the `%left` / `%right UMINUS` precedence table
    instead of term/factor rules
  
  Bison reports **0 conflicts**. Also carried the "missing `;`" error
  productions over from Project 2.
- **Phase 2 — AST (`ast.h`, `ast.c`):** nine new node types (`FUNC_DEF`,
  `PARAM`, `PARAM_LIST`, `FUNC_CALL`, `ARG_LIST`, `RETURN`, `BLOCK`,
  `ARRAY_DECL`, `ARRAY_INDEX`), their constructors, `appendParam` /
  `appendArg` to keep source order, and `printAST` cases for each.
- **Phase 3 — Semantic analysis (`semantic.c`):** replaced the flat scope
  with a scope stack and built a function table seeded with `print/1`. Ran it
  in **two passes** (signatures first, then bodies) so a function can call
  one defined later in the file. The checks cover:
  - wrong argument counts
  - undefined and duplicate functions
  - a scalar passed where an array is expected (and vice versa)
  - indexing a scalar, using an array as a value, assigning a whole array
  - array size 0
  - `return` outside a function
  - more than 4 parameters
  - exactly one `main`

### Christoffer Hansen

- **Phase 4 — Intermediate code (`tac.c`):**
  - TAC for `FUNC_BEGIN/END`, `PARAM`, `ARG`, `CALL`, `RETURN`,
    `ARRAY_DECL`, `ARRAY_LOAD` and `ARRAY_STORE`
  - arguments evaluated before their `ARG`s so nested calls like
    `f(1, g(2))` stay correct
  - **scope renaming** (`x__1`, `x__2`, ...) so a variable declared inside a
    block gets its own frame slot
  - an implicit `main()` so the Topic 2 programs still compile
- **Phase 5 — Optimizer fixes (`tac.c`):** after a `CALL`, the optimizer now
  forgets temporaries that copy a variable the callee may have changed. Also
  fixed `freeTemp("total")` wrongly freeing `t0`.
- **Phase 6 — MIPS code generation (`codegen.c`):**
  - activation records (prologue, epilogue, saved `$ra`)
  - parameters copied from `$a0`–`$a3` into their frame slots
  - calls that load arguments **before** saving the caller's registers
  - `return` that jumps to the epilogue, writing back only dirty globals
  - the three array addressing cases (global, local, by-reference)
  - register pinning so one instruction never evicts its own operands
- **Driver (`main.c`):** lexical errors now fail the compile; syntax errors
  are reported once, as precisely as possible.
- **Tests:** added `t3_06`–`t3_13`:
  - fact(5) = 120 through a six-deep call chain
  - arrays passed by reference
  - nested block scopes
  - expressions large enough to force register spills
  - four failing programs, covering call, array, scope and parameter-limit
    errors
  
  Also brought the 11 Topic 2 tests forward unchanged.
- **Verification:** `run_tests.sh` and `make check` (PASS/FAIL against each
  test's expected output — **24 passed, 0 failed**), and a clean `-Wall` build.
- **Documentation:** README (build steps, phase-by-phase table, activation
  record diagram, test table, design decisions) and this file.

## Our approach to integrating increasingly complex components

1. **Expressions first.** Emma added the operators and precedence table;
   nothing else depends on them, and `t3_01` checked them on their own.
2. **Arrays next, inside the existing program.** Emma's grammar and checks
   plus Christoffer's addressing code let us debug the three addressing cases
   (global, local, by-reference) before functions existed.
3. **Functions last, because they touch every phase.** We agreed the AST
   node layout up front so we could work in parallel: Emma built grammar →
   two-pass semantics, Christoffer built `PARAM/ARG/CALL/RETURN` TAC →
   activation records.
4. **The full suite ran after every merge** (`make check`), so each new
   feature was shown not to break earlier ones, including all of Topic 2.

## Video checklist (each member records their own)

a) **How the code works.** Walk the six phases using
   `./minicompiler tests/t3_06_fact.cm out.s`
   (AST → scope trace → TAC → optimized TAC → frame layout → MIPS).
   - Emma: focus on the grammar, AST and semantic passes.
   - Christoffer: focus on TAC, activation records and the calling
     convention.

b) **Your individually assigned tasks.** Your section above.

c) **The team's integration approach.** The four steps above.

d) **Execution.**
   1. Run `make check` (24 passed).
   2. Run `spim -file out.s` on `t3_06` (fact = 120).
   3. Run one failing test (e.g. `t3_10`) to show the error messages.
