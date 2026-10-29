/* =========================================================================
 * CST-405  ·  TOPIC 3  ·  Compiling Complex Variables and Functions
 * FILE: semantic.c   —   Phase 3 — Semantic analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                               ^^^^^^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • A scope STACK replaces the single flat scope
 *   • Function signatures are collected first, so calls may appear before definitions
 *   • Argument count is checked against the declaration
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 *
 * STATUS
 *   Every TODO (Topic 3) in this file has been implemented.  Each finished
 *   section is marked  DONE (Topic 3)  with a note on the design decision.
 * ========================================================================= */

/* SEMANTIC ANALYSIS IMPLEMENTATION - WITH FUNCTION SUPPORT
 * Performs semantic checks on the Abstract Syntax Tree
 * Now supports functions, scopes, parameters, arrays and return.
 *
 * WHAT THIS PHASE CHECKS (each error is reported with its line number, and
 * analysis keeps going so that EVERY error in the program is listed):
 *   variables   declared before use, not declared twice in one scope,
 *               not a reserved compiler name (t0, L1, x__1, ...)
 *   arrays      size > 0; an array is always indexed when used as a value;
 *               a scalar is never indexed; a whole array is never assigned
 *   functions   defined once; called with the right number of arguments;
 *               array parameters receive arrays and scalar ones scalars;
 *               at most 4 parameters (they travel in $a0-$a3)
 *   return      only inside a function
 *   main        exactly one entry point (explicit main, or the implicit main
 *               built from top-level statements — never both)
 *
 * WHAT IT DELIBERATELY DOES NOT CHECK
 *   Array bounds.  `a[i]` with i out of range is not knowable at compile time
 *   in general, and the language has no run-time check either — exactly like
 *   C.  That is a real design decision (speed and simplicity over safety),
 *   not an oversight.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "semantic.h"
#include "symtab.h"
#include "trace.h"

#define MAX_FUNCTIONS 100
#define MAX_PARAMS 20
#define MAX_SCOPE_DEPTH 10
#define MAX_REG_ARGS 4    /* $a0-$a3: the calling convention in codegen.h */

/* ==========================================================================
 * FUNCTION TABLE                                           DONE (Topic 3)
 * --------------------------------------------------------------------------
 * A second table, for FUNCTIONS.  Variables and functions live in separate
 * namespaces, which is why a program may declare both `int total;` and
 * `int total(...)`.  We record just enough to check a call: the name, the
 * parameter count, and which parameters are arrays.
 * ========================================================================*/
typedef struct {
    char* name;
    int   paramCount;
    int   isArrayParam[MAX_PARAMS];   /* 1 = parameter i is `int a[]`      */
    int   lineno;                     /* where it was defined              */
    int   builtin;                    /* 1 = provided by the language      */
} FuncInfo;

static FuncInfo functions[MAX_FUNCTIONS];
static int functionCount = 0;

/* Scope for variables.
 * Topic 3 adds isArray[] so that errors can say "you indexed a scalar" or
 * "you used an array as a number", and owner so the trace can say whose
 * scope it is. */
typedef struct {
    char* names[MAX_VARS];
    int   isArray[MAX_VARS];
    int   count;
    const char* owner;     /* "global", a function name, or "block" */
} Scope;

/* Global semantic information */
static SemanticInfo semInfo;
static Scope scopes[MAX_SCOPE_DEPTH];
static int scopeDepth = 0;

/* Context: which function body are we inside?  NULL at file scope. */
static const char* currentFunction = NULL;

/* Did the program contain executable statements at file scope?  If so they
 * form an implicit main() (Topic 2 compatibility, see parser.y). */
static int hasTopLevelStmts = 0;

/* --------------------------------------------------------------------------
 * Uniform error banner, so every Topic 3 error looks like the Topic 2 ones.
 * -------------------------------------------------------------------------*/
static void errorHeader(const char* title, int lineno) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ SEMANTIC ERROR - %-41s ║\n", title);
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "  📍 Location: Line %d\n", lineno);
    semInfo.errorCount++;
}

/* Initialize semantic analyzer */
void initSemantic() {
    semInfo.errorCount = 0;
    semInfo.warningCount = 0;
    scopeDepth = 0;
    currentFunction = NULL;
    hasTopLevelStmts = 0;

    /* DONE (Topic 3): reset the function table and seed it with the one
     * function the language provides for free: `print`, one argument.
     * Registering it means `print` is a known name like any other function
     * (and the arity of a PRINT node is checked against this entry). */
    functionCount = 0;
    functions[0].name = strdup("print");
    functions[0].paramCount = 1;
    functions[0].isArrayParam[0] = 0;
    functions[0].lineno = 0;
    functions[0].builtin = 1;
    functionCount = 1;

    trace("SEMANTIC ANALYZER: initialized (built-in: print/1)\n\n");
}

/* Scope management */
static void enterScope(const char* owner) {
    if (scopeDepth >= MAX_SCOPE_DEPTH) {
        fprintf(stderr, "SEMANTIC ERROR: Maximum scope depth exceeded\n");
        semInfo.errorCount++;
        return;
    }
    scopes[scopeDepth].count = 0;
    scopes[scopeDepth].owner = owner;
    scopeDepth++;
}

static void exitScope() {
    if (scopeDepth > 0) {
        /* Free variable names in this scope */
        for (int i = 0; i < scopes[scopeDepth - 1].count; i++) {
            free(scopes[scopeDepth - 1].names[i]);
        }
        scopeDepth--;
    }
}

/* Print current semantic scopes for debugging */
static void printSemanticScopes() {
    trace("\n┌─────────────────────────────────────────────────────────┐\n");
    trace("│ SEMANTIC SCOPE STACK (Depth: %d)                        \n", scopeDepth);
    trace("├─────────────────────────────────────────────────────────┤\n");

    if (scopeDepth == 0) {
        trace("│ (no active scopes)                                      │\n");
    } else {
        for (int depth = 0; depth < scopeDepth; depth++) {
            if (depth == 0) {
                trace("│ Scope[%d] GLOBAL (%d variables)                        \n", depth, scopes[depth].count);
            } else {
                trace("│ Scope[%d] LOCAL (%d variables)                         \n", depth, scopes[depth].count);
                /* DONE (Topic 3): say WHOSE scope this is — a function's
                 * parameter/body scope or a nested { } block. */
                trace("│   owner: %s\n", scopes[depth].owner ? scopes[depth].owner : "?");
            }

            if (scopes[depth].count > 0) {
                trace("│   Variables: ");
                for (int i = 0; i < scopes[depth].count; i++) {
                    trace("%s%s", scopes[depth].names[i], scopes[depth].isArray[i] ? "[]" : "");
                    if (i < scopes[depth].count - 1) trace(", ");
                }
                trace("\n");
            } else {
                trace("│   (empty)\n");
            }
        }
    }
    trace("└─────────────────────────────────────────────────────────┘\n\n");
}

/* RESERVED IDENTIFIERS
 * The intermediate-code generator invents names of its own: temporaries
 * t0, t1, ... and labels L0, L1, ...  If a user variable had one of those
 * shapes the back end could not tell them apart, so the language reserves
 * that namespace.  Reporting it here — in the semantic analyzer, with a line
 * number — is far kinder than a mysterious wrong answer at run time.
 * Topic 3: names containing "__" are reserved too — tac.c renames shadowed
 * block-local variables to  x__1, x__2, ...  (see tac.c, SCOPE RENAMING). */
static int isReservedName(const char* name) {
    if (!name || !name[0]) return 0;
    if (strncmp(name, "__sw", 4) == 0) return 1;          /* switch temporaries */
    if (strstr(name, "__")) return 1;                     /* renamed locals     */
    if (name[0] != 't' && name[0] != 'L') return 0;
    if (!name[1]) return 0;
    for (int i = 1; name[i]; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    return 1;
}

static void reportReserved(const char* name, int lineno) {
    errorHeader("Reserved Identifier", lineno);
    fprintf(stderr, "  ❌ Error: '%s' is reserved for the compiler's own use\n", name);
    fprintf(stderr, "  📖 Note: names of the form t0, t1, ... are three-address-code\n");
    fprintf(stderr, "           temporaries, L0, L1, ... are generated labels, and\n");
    fprintf(stderr, "           names containing \"__\" are used for renamed locals.\n");
    fprintf(stderr, "  💡 Suggestion: rename the variable, for example 'total'\n\n");
}

/* Add a scalar variable to the current scope.  -1 if already declared HERE. */
static int addSymbolToScope(char* name, int isArr) {
    if (scopeDepth == 0) {
        fprintf(stderr, "SEMANTIC ERROR: No scope to add variable to\n");
        return -1;
    }

    Scope* currentScope = &scopes[scopeDepth - 1];

    /* Check if already declared in current scope (shadowing an OUTER scope
     * is fine; redeclaring in the SAME scope is not) */
    for (int i = 0; i < currentScope->count; i++) {
        if (strcmp(currentScope->names[i], name) == 0) {
            return -1;  /* Already declared in this scope */
        }
    }

    /* Add to current scope */
    if (currentScope->count >= MAX_VARS) {
        fprintf(stderr, "SEMANTIC ERROR: Too many variables in scope\n");
        return -1;
    }

    currentScope->names[currentScope->count] = strdup(name);
    currentScope->isArray[currentScope->count] = isArr;
    currentScope->count++;
    return 0;
}

static int addVarToScope(char* name) { return addSymbolToScope(name, 0); }

/* DONE (Topic 3): arrays get their own scope-insertion helper — the storage
 * question is settled later in symtab.c, but the error messages differ. */
static int addArrayToScope(char* name, int lineno) {
    if (addSymbolToScope(name, 1) == -1) {
        errorHeader("Duplicate Array Declaration", lineno);
        fprintf(stderr, "  ❌ Error: '%s' is already declared in the current scope\n", name);
        fprintf(stderr, "  💡 Suggestion: give the array a different name\n");
        fprintf(stderr, "  📖 Note: an array and a scalar share one namespace —\n");
        fprintf(stderr, "           `int a; int a[5];` in one scope is a redeclaration\n\n");
        return -1;
    }
    return 0;
}

/* What does `name` resolve to in the visible scopes?
 *   -1 = not declared,  0 = scalar,  1 = array
 * Searches innermost to outermost, so the nearest declaration wins. */
static int lookupVarKind(const char* name) {
    for (int depth = scopeDepth - 1; depth >= 0; depth--) {
        for (int i = 0; i < scopes[depth].count; i++) {
            if (strcmp(scopes[depth].names[i], name) == 0) {
                return scopes[depth].isArray[i];
            }
        }
    }
    return -1;
}

/* Check if variable is declared in any visible scope */
static int isVarDeclaredInScope(char* name) {
    return lookupVarKind(name) != -1;
}

/* ==========================================================================
 * FUNCTION TABLE OPERATIONS                                DONE (Topic 3)
 * countParams walks the parameter list parser.y built; countArgs walks the
 * argument list at a call site.  Comparing those two numbers is the heart of
 * call checking.
 * ========================================================================*/
static FuncInfo* findFunction(const char* name) {
    for (int i = 0; i < functionCount; i++)
        if (strcmp(functions[i].name, name) == 0) return &functions[i];
    return NULL;
}

static int countParams(ASTNode* params) {
    int n = 0;
    for (ASTNode* p = params; p; p = p->data.param_list.next) n++;
    return n;
}

static int countArgs(ASTNode* args) {
    int n = 0;
    for (ASTNode* a = args; a; a = a->data.arg_list.next) n++;
    return n;
}

/* Record one function signature.  Returns NULL if it is a duplicate. */
static FuncInfo* addFunction(ASTNode* def) {
    const char* name = def->data.func_def.name;
    if (findFunction(name)) return NULL;
    if (functionCount >= MAX_FUNCTIONS) {
        fprintf(stderr, "SEMANTIC ERROR: too many functions (max %d)\n", MAX_FUNCTIONS);
        semInfo.errorCount++;
        return NULL;
    }
    FuncInfo* f = &functions[functionCount++];
    f->name = strdup(name);
    f->paramCount = countParams(def->data.func_def.params);
    f->lineno = def->lineno;
    f->builtin = 0;
    int i = 0;
    for (ASTNode* p = def->data.func_def.params; p && i < MAX_PARAMS;
         p = p->data.param_list.next, i++) {
        f->isArrayParam[i] = (p->data.param_list.param->type == NODE_ARRAY_DECL);
    }
    return f;
}

/* Forward declarations: checkStmt, checkStmtList and checkExpr are mutually
 * recursive, which is exactly what you want when the thing being checked is
 * a tree. */
static void checkStmtList(ASTNode* node);
static void checkExpr(ASTNode* node);

/* --------------------------------------------------------------------------
 * Check a call: does the function exist, is the argument count right, and
 * does each argument have the right KIND (array vs scalar)?
 * -------------------------------------------------------------------------*/
static void checkCall(ASTNode* node) {
    const char* name = node->data.func_call.name;
    FuncInfo* f = findFunction(name);
    int nargs = countArgs(node->data.func_call.args);

    if (!f) {
        errorHeader("Undefined Function", node->lineno);
        fprintf(stderr, "  ❌ Error: call to '%s', which is not defined anywhere\n", name);
        if (isVarDeclaredInScope((char*)name))
            fprintf(stderr, "  📖 Note: '%s' is a VARIABLE here; variables cannot be called\n", name);
        fprintf(stderr, "  💡 Suggestion: define it, e.g.  int %s(...) { ... }\n\n", name);
    } else if (f->paramCount != nargs) {
        /* The single most common mistake a compiler can catch here */
        errorHeader("Wrong Number of Arguments", node->lineno);
        fprintf(stderr, "  ❌ Error: '%s' takes %d argument%s but the call passes %d\n",
                name, f->paramCount, f->paramCount == 1 ? "" : "s", nargs);
        if (!f->builtin)
            fprintf(stderr, "  📖 Note: '%s' is defined on line %d\n", name, f->lineno);
        fprintf(stderr, "\n");
    } else {
        trace("  ✓ Call to '%s' with %d argument(s) (line %d)\n", name, nargs, node->lineno);
    }

    /* Check each argument.  An argument for an ARRAY parameter must be the
     * bare name of an array (it is passed as an address); every other
     * argument is an ordinary expression. */
    int i = 0;
    for (ASTNode* a = node->data.func_call.args; a; a = a->data.arg_list.next, i++) {
        ASTNode* e = a->data.arg_list.expr;
        int wantsArray = f && i < f->paramCount && i < MAX_PARAMS && f->isArrayParam[i];
        if (wantsArray) {
            if (e->type != NODE_VAR || lookupVarKind(e->data.name) != 1) {
                if (e->type == NODE_VAR && lookupVarKind(e->data.name) == -1) {
                    checkExpr(e);   /* reports the undeclared name */
                } else {
                    errorHeader("Array Argument Expected", e->lineno);
                    fprintf(stderr, "  ❌ Error: argument %d of '%s' must be an array name\n",
                            i + 1, name);
                    fprintf(stderr, "  📖 Note: the parameter is declared `int x[]`, so the\n");
                    fprintf(stderr, "           caller must pass an array (by reference)\n\n");
                }
            }
        } else {
            checkExpr(e);   /* NODE_VAR naming an array is rejected in there */
        }
    }
}

/* Check expression for semantic correctness */
static void checkExpr(ASTNode* node) {
    if (!node) return;

    switch(node->type) {
        case NODE_NUM:
            /* Literal numbers are always valid */
            break;

        case NODE_VAR: {
            int kind = lookupVarKind(node->data.name);
            if (kind == -1) {
                fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║ SEMANTIC ERROR - Undeclared Variable                      ║\n");
                fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                fprintf(stderr, "  📍 Location: Line %d\n", node->lineno);
                fprintf(stderr, "  ❌ Error: Variable '%s' is used before being declared\n", node->data.name);
                fprintf(stderr, "  💡 Suggestion: Add a declaration before using this variable:\n");
                fprintf(stderr, "     → int %s;  (add this before line %d)\n", node->data.name, node->lineno);
                fprintf(stderr, "  📖 Note: In C-minus, all variables must be declared before use\n");
                fprintf(stderr, "           (a variable declared inside { } ends at the '}')\n\n");
                semInfo.errorCount++;
            } else if (kind == 1) {
                /* Topic 3: a bare array name is only meaningful as an
                 * argument to an array parameter (checked in checkCall). */
                errorHeader("Array Used as a Value", node->lineno);
                fprintf(stderr, "  ❌ Error: '%s' is an array; use an element such as %s[0]\n",
                        node->data.name, node->data.name);
                fprintf(stderr, "  📖 Note: a whole array may only be passed to an `int x[]` parameter\n\n");
            }
            break;
        }

        case NODE_BINOP:
            checkExpr(node->data.binop.left);
            if (node->data.binop.right) {  /* Unary minus has no right operand */
                checkExpr(node->data.binop.right);
            }
            break;

        /* DONE (Topic 3): a call used for its value. */
        case NODE_FUNC_CALL:
            checkCall(node);
            break;

        /* DONE (Topic 3): an array element.  The array must be declared AND
         * be an array; the index is an expression that must itself check
         * out.  We do NOT check the index is in range (see file header). */
        case NODE_ARRAY_INDEX: {
            int kind = lookupVarKind(node->data.array_index.name);
            if (kind == -1) {
                errorHeader("Undeclared Array", node->lineno);
                fprintf(stderr, "  ❌ Error: '%s' is indexed but was never declared\n",
                        node->data.array_index.name);
                fprintf(stderr, "  💡 Suggestion: declare it, e.g.  int %s[10];\n\n",
                        node->data.array_index.name);
            } else if (kind == 0) {
                errorHeader("Indexing a Non-Array", node->lineno);
                fprintf(stderr, "  ❌ Error: '%s' is a scalar int and cannot be indexed\n\n",
                        node->data.array_index.name);
            }
            checkExpr(node->data.array_index.index);
            break;
        }

        default:
            break;
    }
}

/* Check statement */
static void checkStmt(ASTNode* node) {
    if (!node) return;

    switch(node->type) {
        case NODE_DECL:
            if (isReservedName(node->data.decl.name)) {
                reportReserved(node->data.decl.name, node->lineno);
                break;
            }
            if (addVarToScope(node->data.decl.name) == -1) {
                fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║ SEMANTIC ERROR - Duplicate Variable Declaration           ║\n");
                fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                fprintf(stderr, "  📍 Location: Line %d\n", node->lineno);
                fprintf(stderr, "  ❌ Error: Variable '%s' is already declared in the current scope\n", node->data.decl.name);
                fprintf(stderr, "  💡 Suggestion: Choose a different variable name or remove the duplicate declaration\n");
                fprintf(stderr, "  📖 Note: Each variable can only be declared once per scope\n");
                fprintf(stderr, "     → Use assignment (=) to change the value of an existing variable\n");
                fprintf(stderr, "     → Or use a different name: %s2, my%s, etc.\n\n", node->data.decl.name, node->data.decl.name);
                semInfo.errorCount++;
            } else {
                trace("  ✓ Variable '%s' declared (line %d, scope depth %d)\n",
                      node->data.decl.name, node->lineno, scopeDepth - 1);
            }
            break;

        case NODE_ASSIGN:
            /* DONE (Topic 3): the target may now be an array ELEMENT. */
            if (node->data.assign.arrayLHS) {
                /* arr[i] = v : same checks as reading arr[i] */
                checkExpr(node->data.assign.arrayLHS);
                trace("  ✓ Assignment to '%s[...]' checked (line %d)\n",
                      node->data.assign.arrayLHS->data.array_index.name, node->lineno);
            } else {
                int kind = lookupVarKind(node->data.assign.var);
                if (kind == -1) {
                    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                    fprintf(stderr, "║ SEMANTIC ERROR - Assignment to Undeclared Variable        ║\n");
                    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                    fprintf(stderr, "  📍 Location: Line %d\n", node->lineno);
                    fprintf(stderr, "  ❌ Error: Trying to assign to variable '%s' which hasn't been declared\n", node->data.assign.var);
                    fprintf(stderr, "  💡 Suggestion: Declare the variable before assigning to it:\n");
                    fprintf(stderr, "     → Add this before line %d:\n", node->lineno);
                    fprintf(stderr, "       int %s;\n", node->data.assign.var);
                    fprintf(stderr, "  📖 Remember: Variables must be declared before use in C-minus\n\n");
                    semInfo.errorCount++;
                } else if (kind == 1) {
                    errorHeader("Assignment to Whole Array", node->lineno);
                    fprintf(stderr, "  ❌ Error: '%s' is an array; assign to an element, e.g. %s[0] = ...\n\n",
                            node->data.assign.var, node->data.assign.var);
                } else {
                    trace("  ✓ Assignment to '%s' is valid (line %d)\n", node->data.assign.var, node->lineno);
                }
            }
            checkExpr(node->data.assign.value);
            break;

        case NODE_PRINT: {
            /* print is the built-in seeded in initSemantic: one argument,
             * which the grammar already guarantees. */
            checkExpr(node->data.expr);
            trace("  ✓ Print statement is valid\n");
            break;
        }

        /* DONE (Topic 3): `return` OUTSIDE any function parses (see
         * parser.y top_stmt) but means nothing — that gap between "parses"
         * and "means something" is exactly this phase's territory. */
        case NODE_RETURN:
            if (!currentFunction) {
                errorHeader("Return Outside a Function", node->lineno);
                fprintf(stderr, "  ❌ Error: `return` appears at file scope, not inside a function\n");
                fprintf(stderr, "  💡 Suggestion: move the statements into  int main() { ... }\n\n");
            } else if (!node->data.ret.expr) {
                /* Every function in this language returns int */
                fprintf(stderr, "  ⚠ Warning (line %d): bare `return;` in int function '%s' returns 0\n",
                        node->lineno, currentFunction);
                semInfo.warningCount++;
            } else {
                trace("  ✓ Return from '%s' (line %d)\n", currentFunction, node->lineno);
            }
            checkExpr(node->data.ret.expr);
            break;

        /* DONE (Topic 3): NODE_BLOCK opens a scope and closes it again.
         * Those two calls are what make  int x; { int x; }  legal (inner x
         * shadows outer x) and  { int x; int x; }  an error. */
        case NODE_BLOCK:
            enterScope("block");
            trace("  → entered block scope (line %d)\n", node->lineno);
            checkStmtList(node->data.block.stmt_list);
            printSemanticScopes();
            exitScope();
            trace("  ← left block scope\n");
            break;

        /* A call used as a statement: same checks, value discarded */
        case NODE_FUNC_CALL:
            checkCall(node);
            break;

        case NODE_STMT_LIST:
            /* The grammar is left-recursive, so a statement list can contain
             * another statement list.  Handling that here (not only in
             * checkStmtList) is what lets the recursion bottom out. */
            checkStmtList(node);
            break;

        /* DONE (Topic 3): array declaration.  A real declaration needs a
         * positive size; `int a[0];` is rejected here because nothing
         * downstream would.  (A negative size cannot even parse: the grammar
         * takes a NUM, and NUM has no sign.)  Array PARAMETERS are added by
         * addParamsToScope, not here. */
        case NODE_ARRAY_DECL:
            if (isReservedName(node->data.array_decl.name)) {
                reportReserved(node->data.array_decl.name, node->lineno);
                break;
            }
            if (!node->data.array_decl.isParam && node->data.array_decl.size <= 0) {
                errorHeader("Invalid Array Size", node->lineno);
                fprintf(stderr, "  ❌ Error: array '%s' declared with size %d; size must be at least 1\n\n",
                        node->data.array_decl.name, node->data.array_decl.size);
                break;
            }
            if (addArrayToScope(node->data.array_decl.name, node->lineno) == 0) {
                trace("  ✓ Array '%s[%d]' declared (line %d)\n", node->data.array_decl.name,
                      node->data.array_decl.size, node->lineno);
            }
            break;

        case NODE_FUNC_DEF:
            /* Only reachable if a definition were nested inside a body,
             * which the grammar does not allow. */
            break;

        default:
            break;
    }
}

/* Check statement list */
static void checkStmtList(ASTNode* node) {
    if (!node) return;

    if (node->type == NODE_STMT_LIST) {
        checkStmt(node->data.stmtlist.stmt);
        checkStmtList(node->data.stmtlist.next);
    } else {
        checkStmt(node);
    }
}

/* ==========================================================================
 * THE TWO-PASS STRUCTURE                                   DONE (Topic 3)
 * --------------------------------------------------------------------------
 * Pass 1 records every function signature.  Pass 2 checks the bodies.
 * Two passes and not one, so that a function may call another defined
 * LATER in the file.  (Collapse them into one and `main` can no longer call
 * a helper written below it.)
 * ========================================================================*/

/* Walk the top-level list (left-nested STMT_LISTs) in source order. */
static void forEachTopLevel(ASTNode* node, void (*fn)(ASTNode*)) {
    if (!node) return;
    if (node->type == NODE_STMT_LIST) {
        forEachTopLevel(node->data.stmtlist.stmt, fn);
        forEachTopLevel(node->data.stmtlist.next, fn);
    } else {
        fn(node);
    }
}

/* Put the parameters into the function's (already entered) scope. */
static void addParamsToScope(ASTNode* params) {
    for (ASTNode* p = params; p; p = p->data.param_list.next) {
        ASTNode* prm = p->data.param_list.param;
        int isArr = (prm->type == NODE_ARRAY_DECL);
        char* name = isArr ? prm->data.array_decl.name : prm->data.param.name;
        if (isReservedName(name)) { reportReserved(name, prm->lineno); continue; }
        if (addSymbolToScope(name, isArr) == -1) {
            errorHeader("Duplicate Parameter", prm->lineno);
            fprintf(stderr, "  ❌ Error: parameter '%s' appears twice in one parameter list\n\n", name);
        } else {
            trace("  ✓ Parameter '%s%s' (%s)\n", name, isArr ? "[]" : "",
                  isArr ? "by reference" : "by value");
        }
    }
}

/* Pass 2 for one function: params and body share ONE scope (as in C, so
 * `int f(int a) { int a; }` is a redeclaration), nested blocks get more. */
static void checkFuncDef(ASTNode* def) {
    currentFunction = def->data.func_def.name;
    trace("\n── checking function '%s' (line %d) ──\n", currentFunction, def->lineno);
    enterScope(currentFunction);
    addParamsToScope(def->data.func_def.params);
    ASTNode* body = def->data.func_def.body;
    if (body && body->type == NODE_BLOCK) checkStmtList(body->data.block.stmt_list);
    else checkStmt(body);
    printSemanticScopes();
    exitScope();
    currentFunction = NULL;
}

/* Pass 1 for one top-level item */
static void registerFunction(ASTNode* node) {
    if (node->type != NODE_FUNC_DEF) return;
    const char* name = node->data.func_def.name;
    FuncInfo* existing = findFunction(name);
    if (existing) {
        errorHeader("Duplicate Function Definition", node->lineno);
        if (existing->builtin)
            fprintf(stderr, "  ❌ Error: '%s' is a built-in function and cannot be redefined\n\n", name);
        else
            fprintf(stderr, "  ❌ Error: function '%s' is already defined on line %d\n\n",
                    name, existing->lineno);
        return;
    }
    FuncInfo* f = addFunction(node);
    if (!f) return;
    if (f->paramCount > MAX_REG_ARGS) {
        errorHeader("Too Many Parameters", node->lineno);
        fprintf(stderr, "  ❌ Error: '%s' has %d parameters; at most %d are supported\n",
                name, f->paramCount, MAX_REG_ARGS);
        fprintf(stderr, "  📖 Note: arguments travel in $a0-$a3 in this compiler's calling\n");
        fprintf(stderr, "           convention; stack-passed arguments are not implemented\n\n");
    }
    trace("  ✓ Registered function %s/%d (line %d)\n", name, f->paramCount, node->lineno);
}

static void registerFunctions(ASTNode* root) {
    trace("PASS 1 — collecting function signatures\n");
    forEachTopLevel(root, registerFunction);
    trace("\n");
}

/* Pass 2 for one top-level item: globals are declared in order, function
 * bodies are checked, and file-scope statements (implicit main) are checked
 * in the global scope with no enclosing function. */
static void checkTopLevel(ASTNode* node) {
    switch (node->type) {
        case NODE_FUNC_DEF:
            checkFuncDef(node);
            break;
        case NODE_DECL:
        case NODE_ARRAY_DECL:
            checkStmt(node);              /* scope depth 0 = global */
            break;
        default:
            hasTopLevelStmts = 1;         /* part of the implicit main() */
            currentFunction = NULL;
            checkStmt(node);
            break;
    }
}

static void checkFunctions(ASTNode* root) {
    trace("PASS 2 — checking globals and function bodies\n");
    forEachTopLevel(root, checkTopLevel);

    /* Exactly one entry point */
    FuncInfo* m = findFunction("main");
    if (m && hasTopLevelStmts) {
        errorHeader("Statements Outside main", m->lineno);
        fprintf(stderr, "  ❌ Error: the program defines main() AND has executable statements\n");
        fprintf(stderr, "           at file scope; those statements could never run\n");
        fprintf(stderr, "  💡 Suggestion: move them into main()\n\n");
    } else if (!m && !hasTopLevelStmts) {
        errorHeader("No main Function", 0);
        fprintf(stderr, "  ❌ Error: the program has no entry point\n");
        fprintf(stderr, "  💡 Suggestion: add  int main() { ... return 0; }\n\n");
    } else if (m && m->paramCount != 0) {
        errorHeader("main Takes No Parameters", m->lineno);
        fprintf(stderr, "  ❌ Error: main() is declared with %d parameter(s)\n\n", m->paramCount);
    } else if (!m) {
        trace("  ✓ File-scope statements form an implicit main()\n");
    }
}

int performSemanticAnalysis(ASTNode* root) {
    if (!root) {
        fprintf(stderr, "SEMANTIC ERROR: No AST to analyze\n");
        return -1;
    }

    trace("Running semantic analysis with function support...\n\n");

    /* Enter global scope */
    enterScope("global");
    trace("Entered global scope\n");
    printSemanticScopes();

    /* DONE (Topic 3): the single walk is replaced by TWO passes. */
    registerFunctions(root);   /* pass 1: record every signature */
    checkFunctions(root);      /* pass 2: check every body       */

    trace("Final global scope:\n");
    printSemanticScopes();

    /* Exit global scope */
    exitScope();

    return semInfo.errorCount > 0 ? -1 : 0;
}

/* Print semantic analysis summary */
void printSemanticSummary() {
    trace("═══════════════════════════════════════════\n");
    trace("SEMANTIC ANALYSIS SUMMARY\n");
    trace("═══════════════════════════════════════════\n");
    trace("Functions defined:  %d (+ built-in print)\n", functionCount - 1);
    trace("Errors found:       %d\n", semInfo.errorCount);
    trace("Warnings found:     %d\n", semInfo.warningCount);
    trace("\n");

    if (semInfo.errorCount == 0) {
        trace("✓ Semantic analysis passed - program is semantically correct!\n");
    } else {
        trace("✗ Semantic analysis failed - fix errors before proceeding\n");
    }
    trace("═══════════════════════════════════════════\n\n");
}
