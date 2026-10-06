/* =========================================================================
 * CST-405  ·  TOPIC 2  ·  Compiler for a Starter Language
 * FILE: semantic.c   —   Phase 3 — Semantic analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                               ^^^^^^^^  this file
 *
 * WHAT IS NEW IN TOPIC 2
 *   • Undeclared variables and duplicate declarations, reported with line numbers
 *
 * WHAT COMES NEXT
 *   Topic 3 adds functions, arrays and the rest of arithmetic — and with them, real activation records.
 *
 * YOUR TASK
 *   This is Project 2: the first compiler you build end to end.  Sections
 *   marked  TODO (Topic 2)  are yours.  Everything else — the headers, the
 *   scanner, the driver, the register allocator — is given, because the
 *   point of this project is the six PHASES, not the plumbing between them.
 * ========================================================================= */

/* SEMANTIC ANALYSIS IMPLEMENTATION - WITH FUNCTION SUPPORT
 * Performs semantic checks on the Abstract Syntax Tree
 * Now supports functions, scopes, parameters, control flow
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


/* Scope for variables */
typedef struct {
    char* names[MAX_VARS];
    int count;
} Scope;

/* Global semantic information */
static SemanticInfo semInfo;
static Scope scopes[MAX_SCOPE_DEPTH];
static int scopeDepth = 0;

/* Initialize semantic analyzer */
void initSemantic() {
    semInfo.errorCount = 0;
    semInfo.warningCount = 0;
    scopeDepth = 0;

    trace("SEMANTIC ANALYZER: initialized\n\n");
}

/* Scope management */
static void enterScope() {
    if (scopeDepth >= MAX_SCOPE_DEPTH) {
        fprintf(stderr, "SEMANTIC ERROR: Maximum scope depth exceeded\n");
        semInfo.errorCount++;
        return;
    }
    scopes[scopeDepth].count = 0;
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
            }

            if (scopes[depth].count > 0) {
                trace("│   Variables: ");
                for (int i = 0; i < scopes[depth].count; i++) {
                    trace("%s", scopes[depth].names[i]);
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

/* Add variable to current scope */
/* RESERVED IDENTIFIERS
 * The intermediate-code generator invents names of its own: temporaries
 * t0, t1, ... and labels L0, L1, ...  If a user variable had one of those
 * shapes the back end could not tell them apart, so the language reserves
 * that namespace.  Reporting it here — in the semantic analyzer, with a line
 * number — is far kinder than a mysterious wrong answer at run time. */
static int isReservedName(const char* name) {
    if (!name || !name[0]) return 0;
    if (strncmp(name, "__sw", 4) == 0) return 1;          /* switch temporaries */
    if (name[0] != 't' && name[0] != 'L') return 0;
    if (!name[1]) return 0;
    for (int i = 1; name[i]; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    return 1;
}

static void reportReserved(const char* name, int lineno) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ SEMANTIC ERROR - Reserved Identifier                      ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "  📍 Location: Line %d\n", lineno);
    fprintf(stderr, "  ❌ Error: '%s' is reserved for the compiler's own use\n", name);
    fprintf(stderr, "  📖 Note: names of the form t0, t1, ... are three-address-code\n");
    fprintf(stderr, "           temporaries and L0, L1, ... are generated labels.\n");
    fprintf(stderr, "  💡 Suggestion: rename the variable, for example '%s_' or 'total'\n\n", name);
}

static int addVarToScope(char* name) {
    if (scopeDepth == 0) {
        fprintf(stderr, "SEMANTIC ERROR: No scope to add variable to\n");
        return -1;
    }

    Scope* currentScope = &scopes[scopeDepth - 1];

    /* Check if already declared in current scope */
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
    currentScope->count++;
    return 0;
}

/* Check if variable is declared in any visible scope */
static int isVarDeclaredInScope(char* name) {
    /* Search from innermost to outermost scope */
    for (int depth = scopeDepth - 1; depth >= 0; depth--) {
        for (int i = 0; i < scopes[depth].count; i++) {
            if (strcmp(scopes[depth].names[i], name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* Forward declaration: checkStmt and checkStmtList are mutually recursive,
 * which is exactly what you want when the thing being checked is a tree. */
static void checkStmtList(ASTNode* node);

/* Check expression for semantic correctness */
static void checkExpr(ASTNode* node) {
    /* ----------------------------------------------------------------
     * TODO (Topic 2) — CHECK AN EXPRESSION -- Finished
     * Walk the expression and report anything that cannot mean what it says.
     *
     *     NODE_NUM    always fine
     *     NODE_VAR    the name must be declared and VISIBLE here
     *                 -> isVarDeclaredInScope(node->data.name)
     *     NODE_BINOP  nothing to check about the operator itself; recurse into
     *                 both operands
     *
     * When you report an error: give the LINE NUMBER (node->lineno), name the
     * identifier, and say what would fix it.  Compare these two messages and
     * decide which one you would rather receive:
     *
     *     error: undeclared identifier
     *     line 7: 'totl' is not declared — did you mean 'total'?
     *
     * Increment semInfo.errorCount for each error.  Do NOT stop at the first
     * one: report everything you can find in a single run.
     * ---------------------------------------------------------------- */
    if (!node) return;
 
    switch (node->type) {
        case NODE_NUM:
            break;                                   /* always fine */
 
        case NODE_VAR:
            if (!isVarDeclaredInScope(node->data.name)) {
                fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║ SEMANTIC ERROR - Undeclared Variable                      ║\n");
                fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                fprintf(stderr, "  Location: Line %d\n", node->lineno);
                fprintf(stderr, "  Error: variable '%s' is used but never declared\n", node->data.name);
                fprintf(stderr, "  Suggestion: declare it first, e.g. 'int %s;'\n\n", node->data.name);
                semInfo.errorCount++;
            }
            break;
 
        case NODE_BINOP:
            checkExpr(node->data.binop.left);
            checkExpr(node->data.binop.right);
            break;
 
        default:
            break;
    }
}

/* Check statement */
static void checkStmt(ASTNode* node) {
    /* ----------------------------------------------------------------
     * TODO (Topic 2) — CHECK A STATEMENT -- Finished
     *     NODE_DECL    the name must NOT already be declared in this scope.
     *                  On success, add it: addVarToScope(name).
     *     NODE_ASSIGN  the target must already be declared; then check the
     *                  expression on the right with checkExpr.
     *     NODE_PRINT   check the expression.
     *     NODE_STMT_LIST  recurse into both halves via checkStmtList.
     *
     * Order matters in NODE_ASSIGN and it is easy to get backwards.  For
     *     int x;  x = x + 1;
     * checking the right-hand side must happen with x already in scope.  For
     *     int x = x + 1;
     * (a form this language does not have — but think about it) it should not.
     * Languages differ here, and this is where that decision gets made.
     * ---------------------------------------------------------------- */
    if (!node) return;
 
    switch (node->type) {
        case NODE_DECL: {
            char* name = node->data.decl.name;
 
            if (isReservedName(name)) {              /* t0, L1, ... */
                reportReserved(name, node->lineno);
                semInfo.errorCount++;
                break;
            }
            if (addVarToScope(name) != 0) {          /* -1: already declared */
                fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║ SEMANTIC ERROR - Duplicate Declaration                    ║\n");
                fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                fprintf(stderr, "  Location: Line %d\n", node->lineno);
                fprintf(stderr, "  Error: variable '%s' is already declared in this scope\n", name);
                fprintf(stderr, "  Suggestion: remove the second declaration or pick a different name\n\n");
                semInfo.errorCount++;
            }
            break;
        }
 
        case NODE_ASSIGN:
            /* The target must exist.  The right-hand side is checked with the
             * current scope, so  int x; x = x + 1;  is fine. */
            if (!isVarDeclaredInScope(node->data.assign.var)) {
                fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║ SEMANTIC ERROR - Assignment to Undeclared Variable        ║\n");
                fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                fprintf(stderr, "  Location: Line %d\n", node->lineno);
                fprintf(stderr, "  Error: cannot assign to '%s' because it is not declared\n",
                        node->data.assign.var);
                fprintf(stderr, "  Suggestion: declare it first, e.g. 'int %s;'\n\n",
                        node->data.assign.var);
                semInfo.errorCount++;
            }
            checkExpr(node->data.assign.value);      /* keep looking: report everything */
            break;
 
        case NODE_PRINT:
            checkExpr(node->data.expr);
            break;
 
        case NODE_STMT_LIST:
            checkStmtList(node);
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
 
int performSemanticAnalysis(ASTNode* root) {
    if (!root) {
        fprintf(stderr, "SEMANTIC ERROR: No AST to analyze\n");
        return -1;
    }
 
    trace("Running semantic analysis with function support...\n\n");
 
    /* Enter global scope */
    enterScope();
    trace("Entered global scope\n");
    printSemanticScopes();
 
    /* The starter language has no functions, so one walk over the statement
     * list is the whole analysis.  Topic 3 replaces this with two passes. */
    checkStmtList(root);
 
    /* Exit global scope */
    exitScope();
 
    return semInfo.errorCount > 0 ? -1 : 0;
}
 
/* Print semantic analysis summary */
void printSemanticSummary() {
    trace("═══════════════════════════════════════════\n");
    trace("SEMANTIC ANALYSIS SUMMARY\n");
    trace("═══════════════════════════════════════════\n");
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
 
