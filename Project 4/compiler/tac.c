/* =========================================================================
 * CST-405  ·  TOPIC 4  ·  Compiling Loops
 * FILE: tac.c   —   Phases 4 & 5 — Intermediate code and optimization
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                                           ^^^  this file
 *
 * WHAT IS NEW IN TOPIC 4
 *   • Labels and jumps: LABEL, GOTO, IF_FALSE, IF_TRUE
 *   • Loops are lowered to test-at-the-top label/jump patterns
 *   • A break-label stack so `break` knows which loop to leave
 *   • The optimizer now runs to a FIXED POINT and reports what each technique achieved
 *
 * WHAT COMES NEXT
 *   Topic 5 adds decisions (if, if-else, switch) and the logical operators.
 *
 * STATUS
 *   Topic 4 loop lowering and branch simplification are implemented.
 *   Project 4 also restores scope renaming and the implicit main, fixes the
 *   evaluation order, and adds loop-aware optimizations (techniques 8-10).
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "tac.h"
#include "trace.h"

TACList tacList;
TACList optimizedList;
TempAllocator tempAlloc;

void initTAC() {
    tacList.head = NULL;
    tacList.tail = NULL;
    tacList.tempCount = 0;
    tacList.labelCount = 0;
    optimizedList.head = NULL;
    optimizedList.tail = NULL;

    /* Initialize temporary allocator */
    for (int i = 0; i < MAX_TEMPS; i++) {
        tempAlloc.allocated[i] = 0;
    }
    tempAlloc.maxUsed = 0;
    tempAlloc.freeCount = 0;
    trace("TAC: Temporary allocator initialized\n");
}

char* newTemp() {
    char* temp = malloc(10);
    sprintf(temp, "t%d", tacList.tempCount++);
    return temp;
}

char* allocTemp() {
    int tempNum;

    if (tempAlloc.freeCount > 0) {
        tempNum = tempAlloc.freeList[--tempAlloc.freeCount];
        trace("    [TAC ALLOC] Reusing temporary t%d\n", tempNum);
    } else {
        tempNum = tempAlloc.maxUsed++;
        if (tempNum >= MAX_TEMPS) {
            fprintf(stderr, "ERROR: Exceeded maximum temporaries\n");
            exit(1);
        }
        trace("    [TAC ALLOC] Allocating new temporary t%d\n", tempNum);
    }

    tempAlloc.allocated[tempNum] = 1;
    char* temp = malloc(10);
    sprintf(temp, "t%d", tempNum);
    return temp;
}

void freeTemp(char* temp) {
    if (!temp || temp[0] != 't') return;

    int tempNum = atoi(temp + 1);
    if (!tempAlloc.allocated[tempNum]) return;

    tempAlloc.allocated[tempNum] = 0;
    if (tempAlloc.freeCount < MAX_TEMPS) {
        tempAlloc.freeList[tempAlloc.freeCount++] = tempNum;
        trace("    [TAC FREE] Released temporary %s\n", temp);
    }
}

/* Hand out a fresh jump target: "L0", "L1", ...  Returns a malloc'd name.
 * The counter lives in tacList and only ever increases, so two labels can
 * never collide, even for nested loops.  (A repeated label is an assembler
 * error at best and silently wrong code at worst.)  Labels use a different
 * namespace from temps ("t<N>") and user variables cannot start with 'L'
 * followed by digits only by accident of naming: known limitation. */
char* newLabel() {
    char* label = malloc(16);
    sprintf(label, "L%d", tacList.labelCount++);
    return label;
}

void printTempAllocatorState() {
    trace("\n┌──────────────────────────────────────────────────────────┐\n");
    trace("│ TEMPORARY ALLOCATOR STATISTICS                           │\n");
    trace("├──────────────────────────────────────────────────────────┤\n");
    trace("│ Total temporaries used:      %3d                         │\n", tempAlloc.maxUsed);
    trace("│ Currently allocated:         %3d                         │\n",
           tempAlloc.maxUsed - tempAlloc.freeCount);
    trace("│ Available for reuse:         %3d                         │\n", tempAlloc.freeCount);
    trace("└──────────────────────────────────────────────────────────┘\n\n");
}

TACInstr* createTAC(TACOp op, char* arg1, char* arg2, char* result) {
    TACInstr* instr = malloc(sizeof(TACInstr));
    instr->op = op;
    instr->arg1 = arg1 ? strdup(arg1) : NULL;
    instr->arg2 = arg2 ? strdup(arg2) : NULL;
    instr->result = result ? strdup(result) : NULL;
    instr->next = NULL;
    return instr;
}

void appendTAC(TACInstr* instr) {
    if (!tacList.head) {
        tacList.head = tacList.tail = instr;
    } else {
        tacList.tail->next = instr;
        tacList.tail = instr;
    }
}

void appendOptimizedTAC(TACInstr* instr) {
    if (!optimizedList.head) {
        optimizedList.head = optimizedList.tail = instr;
    } else {
        optimizedList.tail->next = instr;
        optimizedList.tail = instr;
    }
}

/* ── BREAK LABEL STACK ──────────────────────────────────────────────────────
 * When entering a switch, for, or while, push the "end" label so that any
 * nested NODE_BREAK can emit GOTO to the right exit point.
 */
#define MAX_LOOP_DEPTH 32          /* Known limitation: loops nested deeper than this are rejected */
static char* breakLabels[MAX_LOOP_DEPTH];
static int   breakTop = 0;

/* Push the exit label of the loop whose BODY is about to be generated. */
static void pushBreakLabel(char* label) {
    if (breakTop >= MAX_LOOP_DEPTH) {
        fprintf(stderr, "TAC ERROR: loops nested more than %d deep\n", MAX_LOOP_DEPTH);
        exit(1);
    }
    breakLabels[breakTop++] = label;
}

/* Pop it once the body is done.  Because it is a stack, `break` inside an
 * inner loop sees the inner label, and after the inner loop ends the outer
 * label is on top again: "innermost" falls out of the data structure. */
static void popBreakLabel(void) { if (breakTop > 0) breakTop--; }

/* ==========================================================================
 * SCOPE RENAMING                      (restored from our Project 3 work)
 * --------------------------------------------------------------------------
 * semantic.c uses a scope STACK, but the code generator's storage map
 * (symtab.c) is FLAT per function: one slot per name.  So
 *
 *      int main() { int i; i = 0; while (i < 3) { int i; i = 50; } print(i); }
 *
 * would give both i's ONE slot and print 50 instead of 0.  The fix is the
 * one real compilers use: give every declaration a unique name on the way
 * into TAC.  The first declaration of a name in a function keeps it; a later
 * one that would collide (with an earlier local, or with a global it must
 * not hide for the rest of the function) becomes  i__1, i__2, ...  ("__" is
 * reserved by semantic.c so a user name can never clash with a renamed one).
 * References are resolved innermost-first, so each use gets the right slot.
 * Loop bodies are BLOCKs, so a variable declared inside a loop body gets its
 * own slot and stops being visible at the loop's closing '}'.
 * ========================================================================*/
#define MAX_TAC_NAMES 512

typedef struct { const char* src; char* uniq; } NameBinding;

static NameBinding bindings[MAX_TAC_NAMES];      /* visible locals, innermost last */
static int bindingCount = 0;

static const char* declaredInFunc[MAX_TAC_NAMES]; /* every local name seen so far   */
static int declaredCount = 0;

static const char* globalNames[MAX_TAC_NAMES];    /* every file-scope name          */
static int globalCount = 0;

static void resetFunctionNames(void) { bindingCount = 0; declaredCount = 0; }

static int isGlobalName(const char* n) {
    for (int i = 0; i < globalCount; i++) if (strcmp(globalNames[i], n) == 0) return 1;
    return 0;
}

/* Declare a local and return its storage name.  isParam: parameters are
 * visible for the WHOLE body, so a parameter shadowing a global needs no
 * rename (the global is simply unreachable inside that function). */
static const char* declareLocal(const char* src, int isParam) {
    int seen = 0;
    for (int i = 0; i < declaredCount; i++)
        if (strcmp(declaredInFunc[i], src) == 0) seen++;
    int clash = seen > 0 || (!isParam && isGlobalName(src));

    char* uniq;
    if (!clash) {
        uniq = strdup(src);
    } else {
        uniq = malloc(strlen(src) + 16);
        sprintf(uniq, "%s__%d", src, seen + 1);
        trace("    [SCOPE] local '%s' renamed to '%s' (shadows an outer '%s')\n",
              src, uniq, src);
    }
    if (declaredCount < MAX_TAC_NAMES) declaredInFunc[declaredCount++] = src;
    if (bindingCount < MAX_TAC_NAMES) {
        bindings[bindingCount].src  = src;
        bindings[bindingCount].uniq = uniq;
        bindingCount++;
    }
    return uniq;
}

/* Which storage name does a source name refer to here? */
static const char* resolveName(const char* src) {
    for (int i = bindingCount - 1; i >= 0; i--)
        if (strcmp(bindings[i].src, src) == 0) return bindings[i].uniq;
    return src;                                  /* not local: a global */
}

/* Forward declarations */
static void generateTACStmt(ASTNode* node);
static int generateArgTAC(ASTNode* args);

/* ==========================================================================
 * EVALUATION ORDER: STRICTLY LEFT TO RIGHT          (design choice, Project 4)
 * --------------------------------------------------------------------------
 * C leaves the order of operand evaluation unspecified; this language
 * defines it.  generateTACExpr returns a bare variable NAME (not a copy) for
 * an operand like g, so in  g + f(0)  the read of g would really happen when
 * the ADD executes -- AFTER f has run and perhaps changed g.  Rule: when a
 * later operand (or later argument) contains a call, an earlier operand that
 * is a plain variable is first copied into a temporary.  The optimizer keeps
 * that copy, because after a CALL it forgets every temporary that copies a
 * variable (see dropNonTempFacts).  Found by differential fuzzing.
 * ========================================================================*/
static int containsCall(ASTNode* n) {
    if (!n) return 0;
    switch (n->type) {
        case NODE_FUNC_CALL:   return 1;
        case NODE_BINOP:       return containsCall(n->data.binop.left) ||
                                      containsCall(n->data.binop.right);
        case NODE_ARRAY_INDEX: return containsCall(n->data.array_index.index);
        case NODE_ARG_LIST:    return containsCall(n->data.arg_list.expr) ||
                                      containsCall(n->data.arg_list.next);
        default:               return 0;
    }
}

/* Is this TAC operand a name the program can change (not a temp/literal)? */
static int isVariableOperand(const char* s) {
    if (!s || !*s) return 0;
    if ((s[0] >= '0' && s[0] <= '9') || s[0] == '-') return 0;      /* literal */
    if (s[0] == 't' && s[1] >= '0' && s[1] <= '9') {
        int k = 1; while (s[k] >= '0' && s[k] <= '9') k++;
        if (!s[k]) return 0;                                        /* temp    */
    }
    return 1;
}

/* Pin down the CURRENT value of a variable operand in a fresh temporary. */
static char* snapshot(char* operand) {
    if (!isVariableOperand(operand)) return operand;
    char* t = allocTemp();
    appendTAC(createTAC(TAC_ASSIGN, operand, NULL, t));
    trace("    [ORDER] '%s' copied to %s: a call to its right may change it\n", operand, t);
    free(operand);
    return t;
}

/* Flatten a (left-nested) ARG_LIST into source order: out[0] is argument 1. */
#define MAX_CALL_ARGS 32
static int flattenArgList(ASTNode* a, ASTNode** out, int n) {
    if (!a) return n;
    if (a->type == NODE_ARG_LIST) {
        n = flattenArgList(a->data.arg_list.expr, out, n);
        return flattenArgList(a->data.arg_list.next, out, n);
    }
    if (n < MAX_CALL_ARGS) out[n] = a;
    return n + 1;
}

/* Generate TAC for a call's arguments, left to right; returns the count.
 * Each argument is evaluated and then ARG'd.  codegen.c reads ARG operands
 * only at the CALL, so an argument that is a plain variable is snapshotted
 * when a LATER argument contains a call (see EVALUATION ORDER). */
static int generateArgTAC(ASTNode* args) {
    ASTNode* list[MAX_CALL_ARGS];
    int n = flattenArgList(args, list, 0);
    if (n > MAX_CALL_ARGS) n = MAX_CALL_ARGS;
    for (int k = 0; k < n; k++) {
        int laterCall = 0;
        for (int m = k + 1; m < n; m++) if (containsCall(list[m])) laterCall = 1;
        char* argTemp = generateTACExpr(list[k]);
        if (laterCall) argTemp = snapshot(argTemp);
        appendTAC(createTAC(TAC_ARG, argTemp, NULL, NULL));
    }
    return n;
}


/* Generate TAC for expression - returns the temp/var holding result */
char* generateTACExpr(ASTNode* node) {
    if (!node) return NULL;

    switch(node->type) {
        case NODE_NUM: {
            char* temp = malloc(20);
            sprintf(temp, "%d", node->data.num);
            return temp;
        }

        case NODE_VAR:
            return strdup(resolveName(node->data.name));

        case NODE_BINOP: {
            char* left = generateTACExpr(node->data.binop.left);
            char* right = NULL;

            /* Unary operators have no right operand: unary minus from
             * Topic 3, and logical NOT from Topic 5. */
            if (node->data.binop.op != 'u' && node->data.binop.op != '!') {
                if (containsCall(node->data.binop.right)) left = snapshot(left);
                right = generateTACExpr(node->data.binop.right);
            }

            char* temp = allocTemp();

            /* Select operation type */
            TACOp op;
            switch(node->data.binop.op) {
                case '+': op = TAC_ADD; break;
                case '-': op = TAC_SUB; break;
                case '*': op = TAC_MUL; break;
                case '/': op = TAC_DIV; break;
                case '<': op = TAC_LT; break;
                case '>': op = TAC_GT; break;
                case 'l': op = TAC_LE; break;   /* <= */
                case 'g': op = TAC_GE; break;   /* >= */
                case 'e': op = TAC_EQ; break;   /* == */
                case 'n': op = TAC_NE; break;   /* != */
                case 'u': op = TAC_NEG; break; /* unary - */
                default:  op = TAC_ADD; break;
            }

            appendTAC(createTAC(op, left, right, temp));

            freeTemp(left);
            if (right) freeTemp(right);

            return temp;
        }

        case NODE_FUNC_CALL: {
            /* Generate TAC for arguments and count them */
            int argCount = generateArgTAC(node->data.func_call.args);

            /* Generate call instruction */
            char* result = allocTemp();
            char argCountStr[10];
            sprintf(argCountStr, "%d", argCount);
            appendTAC(createTAC(TAC_CALL, node->data.func_call.name, argCountStr, result));

            return result;
        }

        case NODE_ARRAY_INDEX: {
            /* Generate index expression */
            char* indexTemp = generateTACExpr(node->data.array_index.index);

            /* Allocate temporary for result */
            char* resultTemp = allocTemp();

            /* Generate TAC_ARRAY_LOAD: result = array[index] */
            appendTAC(createTAC(TAC_ARRAY_LOAD, (char*)resolveName(node->data.array_index.name),
                                indexTemp, resultTemp));

            freeTemp(indexTemp);
            return resultTemp;
        }

        default:
            return NULL;
    }
}

/* Generate TAC for statement list */
static void generateTACStmtList(ASTNode* node) {
    if (!node) return;

    if (node->type == NODE_STMT_LIST) {
        generateTACStmt(node->data.stmtlist.stmt);
        generateTACStmtList(node->data.stmtlist.next);
    } else {
        generateTACStmt(node);
    }
}

/* Generate TAC for a statement */
static void generateTACStmt(ASTNode* node) {
    if (!node) return;

    switch(node->type) {
        case NODE_DECL: {
            /* Inside a function every declaration is a LOCAL (renamed if it
             * would collide; see SCOPE RENAMING).  File-scope declarations
             * never get here: generateTopItem emits them as globals. */
            const char* name = declareLocal(node->data.decl.name, 0);
            appendTAC(createTAC(TAC_DECL, node->data.decl.varType, NULL, (char*)name));
            break;
        }

        case NODE_ASSIGN: {
            if (node->data.assign.arrayLHS) {
                /* Array element assignment: arr[index] = expr.  Left to right
                 * (as in our Project 3 compiler): the index first, then the
                 * value, so in  a[i] = f()  the index is the i from BEFORE the
                 * call even if f changes i. */
                ASTNode* arrayIndex = node->data.assign.arrayLHS;
                char* indexTemp = generateTACExpr(arrayIndex->data.array_index.index);
                if (containsCall(node->data.assign.value)) indexTemp = snapshot(indexTemp);
                char* expr = generateTACExpr(node->data.assign.value);

                /* Generate TAC_ARRAY_STORE: array[index] = value */
                appendTAC(createTAC(TAC_ARRAY_STORE,
                                    (char*)resolveName(arrayIndex->data.array_index.name),
                                    indexTemp, expr));

                freeTemp(indexTemp);
                freeTemp(expr);
            } else {
                /* Regular assignment */
                char* expr = generateTACExpr(node->data.assign.value);
                appendTAC(createTAC(TAC_ASSIGN, expr, NULL,
                                    (char*)resolveName(node->data.assign.var)));
                freeTemp(expr);
            }
            break;
        }

        case NODE_PRINT: {
            char* expr = generateTACExpr(node->data.expr);
            appendTAC(createTAC(TAC_PRINT, expr, NULL, NULL));
            freeTemp(expr);
            break;
        }

        case NODE_RETURN: {
            if (node->data.ret.expr) {
                char* expr = generateTACExpr(node->data.ret.expr);
                appendTAC(createTAC(TAC_RETURN, expr, NULL, NULL));
                freeTemp(expr);
            } else {
                appendTAC(createTAC(TAC_RETURN, NULL, NULL, NULL));
            }
            break;
        }

        /* while (c) B   lowers to
         *     Lstart:  t = c ; IF_FALSE t GOTO Lend ; B ; GOTO Lstart
         *     Lend:
         * The test is at the TOP, so a condition that starts false runs the
         * body zero times.  The back edge (GOTO Lstart) is what makes this a
         * loop and not a straight line. */
        case NODE_WHILE: {
            char* Lstart = newLabel();
            char* Lend   = newLabel();

            appendTAC(createTAC(TAC_LABEL, NULL, NULL, Lstart));
            char* cond = generateTACExpr(node->data.while_loop.cond);
            appendTAC(createTAC(TAC_IF_FALSE, cond, Lend, NULL));
            freeTemp(cond);

            pushBreakLabel(Lend);          /* only the body is "inside" for break */
            generateTACStmt(node->data.while_loop.body);
            popBreakLabel();

            appendTAC(createTAC(TAC_GOTO, Lstart, NULL, NULL));
            appendTAC(createTAC(TAC_LABEL, NULL, NULL, Lend));
            free(Lstart); free(Lend);
            break;
        }

        /* for (init; c; upd) B   lowers DIRECTLY (not desugared to while):
         *     <init>
         *     Lstart:  t = c ; IF_FALSE t GOTO Lend ; B ; <upd> ; GOTO Lstart
         *     Lend:
         * DESIGN CHOICE: direct lowering costs a few extra lines here, but
         * keeps the update at a known place (right before the back edge).
         * That matters if `continue` is ever added: it must jump to <upd>,
         * which a naive desugaring into `while` with the update at the end
         * of the body would skip.  A NULL condition means "always true", so
         * the test is simply not emitted and only break can leave. */
        case NODE_FOR: {
            char* Lstart = newLabel();
            char* Lend   = newLabel();

            generateTACStmt(node->data.for_loop.init);      /* NULL-safe: runs once */

            appendTAC(createTAC(TAC_LABEL, NULL, NULL, Lstart));
            if (node->data.for_loop.cond) {
                char* cond = generateTACExpr(node->data.for_loop.cond);
                appendTAC(createTAC(TAC_IF_FALSE, cond, Lend, NULL));
                freeTemp(cond);
            }

            pushBreakLabel(Lend);
            generateTACStmt(node->data.for_loop.body);
            popBreakLabel();

            generateTACStmt(node->data.for_loop.update);    /* NULL-safe */
            appendTAC(createTAC(TAC_GOTO, Lstart, NULL, NULL));
            appendTAC(createTAC(TAC_LABEL, NULL, NULL, Lend));
            free(Lstart); free(Lend);
            break;
        }

        /* A block recurses into its statement list.  Names declared inside it
         * stop being visible at the '}', so pop their bindings. */
        case NODE_BLOCK: {
            int mark = bindingCount;
            generateTACStmtList(node->data.block.stmt_list);
            bindingCount = mark;
            break;
        }

        case NODE_FUNC_CALL:
            /* Function call as statement (result discarded) */
            generateTACExpr(node);
            break;


        case NODE_STMT_LIST:
            /* Left recursion in the grammar means a statement list may
             * contain another statement list; handle that here too. */
            generateTACStmtList(node);
            break;

        case NODE_ARRAY_DECL: {
            /* Generate array declaration */
            if (!node->data.array_decl.isParam) {
                const char* name = declareLocal(node->data.array_decl.name, 0);
                char sizeStr[20];
                sprintf(sizeStr, "%d", node->data.array_decl.size);
                appendTAC(createTAC(TAC_ARRAY_DECL, sizeStr, NULL, (char*)name));
            } else {
                const char* name = declareLocal(node->data.array_decl.name, 1);
                /* Array parameter: the slot holds a base ADDRESS, not the
                 * elements.  "int[]" tells the code generator to treat it as
                 * a reference rather than reserving size*4 bytes. */
                appendTAC(createTAC(TAC_PARAM, "int[]", NULL, (char*)name));
            }
            break;
        }

        case NODE_BREAK:
            /* Jump to the exit label of the innermost enclosing loop. */
            if (breakTop == 0) {
                /* Unreachable if semantic analysis ran: belt and braces. */
                fprintf(stderr, "TAC ERROR (line %d): break outside a loop\n", node->lineno);
                exit(1);
            }
            appendTAC(createTAC(TAC_GOTO, breakLabels[breakTop - 1], NULL, NULL));
            break;

        default:
            break;
    }
}

/* Recursively generate TAC for function parameters */
static void generateParamTAC(ASTNode* param) {
    if (!param) return;

    if (param->type == NODE_PARAM) {
        const char* name = declareLocal(param->data.param.name, 1);
        appendTAC(createTAC(TAC_PARAM, "int", NULL, (char*)name));
    } else if (param->type == NODE_ARRAY_DECL && param->data.array_decl.isParam) {
        const char* name = declareLocal(param->data.array_decl.name, 1);
        appendTAC(createTAC(TAC_PARAM, "int[]", NULL, (char*)name));
    } else if (param->type == NODE_PARAM_LIST) {
        // Recursively process nested param lists
        generateParamTAC(param->data.param_list.param);
        generateParamTAC(param->data.param_list.next);
    }
}

/* Generate TAC for function definition */
static void generateTACFuncDef(ASTNode* node) {
    if (!node || node->type != NODE_FUNC_DEF) return;

    resetFunctionNames();                      /* fresh activation record */

    /* Function begin marker */
    appendTAC(createTAC(TAC_FUNC_BEGIN, NULL, NULL, node->data.func_def.name));

    /* Generate parameter declarations */
    generateParamTAC(node->data.func_def.params);

    /* Generate function body.  Params and the body's outermost declarations
     * share one scope (as in C, and as semantic.c checks it), so the body's
     * statements are walked directly rather than as a BLOCK. */
    ASTNode* body = node->data.func_def.body;
    if (body && body->type == NODE_BLOCK) generateTACStmtList(body->data.block.stmt_list);
    else generateTACStmt(body);

    /* Function end marker */
    appendTAC(createTAC(TAC_FUNC_END, NULL, NULL, node->data.func_def.name));
}

/* ==========================================================================
 * THE PROGRAM                          (implicit main restored from Project 3)
 * --------------------------------------------------------------------------
 * Every function gets its own FUNC_BEGIN / FUNC_END pair.  File-scope
 * declarations are emitted OUTSIDE any function, which is how codegen.c
 * recognises them as .data globals.  A Topic 2 style program (statements at
 * file scope, no main) still works: its statements, loops included, are
 * wrapped in an implicit main(); semantic.c has already rejected a program
 * that has both.
 * ========================================================================*/

/* Walk the top-level list (left-nested STMT_LISTs) in source order. */
static void forEachTop(ASTNode* node, void (*fn)(ASTNode*)) {
    if (!node) return;
    if (node->type == NODE_STMT_LIST) {
        forEachTop(node->data.stmtlist.stmt, fn);
        forEachTop(node->data.stmtlist.next, fn);
    } else {
        fn(node);
    }
}

static int hasTopLevelStmts = 0;

/* Pre-pass: remember every global name (needed by SCOPE RENAMING). */
static void collectGlobal(ASTNode* n) {
    const char* name = NULL;
    if (n->type == NODE_DECL) name = n->data.decl.name;
    else if (n->type == NODE_ARRAY_DECL) name = n->data.array_decl.name;
    if (name && globalCount < MAX_TAC_NAMES) globalNames[globalCount++] = name;
}

/* Pass A: global declarations and function definitions. */
static void generateTopItem(ASTNode* n) {
    switch (n->type) {
        case NODE_DECL:
            appendTAC(createTAC(TAC_DECL, n->data.decl.varType, NULL, n->data.decl.name));
            break;
        case NODE_ARRAY_DECL: {
            char size[16];
            snprintf(size, sizeof size, "%d", n->data.array_decl.size);
            appendTAC(createTAC(TAC_ARRAY_DECL, size, NULL, n->data.array_decl.name));
            break;
        }
        case NODE_FUNC_DEF:
            generateTACFuncDef(n);
            break;
        default:
            hasTopLevelStmts = 1;              /* handled in pass B */
            break;
    }
}

/* Pass B: file-scope statements become the body of an implicit main(). */
static void generateTopStmt(ASTNode* n) {
    if (n->type == NODE_DECL || n->type == NODE_ARRAY_DECL || n->type == NODE_FUNC_DEF) return;
    generateTACStmt(n);
}

/* Main TAC generation entry point */
void generateTAC(ASTNode* node) {
    if (!node) return;
    globalCount = 0;
    hasTopLevelStmts = 0;
    forEachTop(node, collectGlobal);
    forEachTop(node, generateTopItem);

    if (hasTopLevelStmts) {
        trace("    [MAIN] file-scope statements wrapped in an implicit main()\n");
        resetFunctionNames();
        appendTAC(createTAC(TAC_FUNC_BEGIN, NULL, NULL, "main"));
        forEachTop(node, generateTopStmt);
        appendTAC(createTAC(TAC_RETURN, "0", NULL, NULL));
        appendTAC(createTAC(TAC_FUNC_END, NULL, NULL, "main"));
    }
}

/* Get operation name for printing */
static const char* getOpName(TACOp op) {
    switch(op) {
        case TAC_ADD: return "+";
        case TAC_SUB: return "-";
        case TAC_MUL: return "*";
        case TAC_DIV: return "/";
        case TAC_NEG: return "NEG";
        case TAC_LT: return "<";
        case TAC_GT: return ">";
        case TAC_LE: return "<=";
        case TAC_GE: return ">=";
        case TAC_EQ: return "==";
        case TAC_NE: return "!=";
        case TAC_AND: return "&&";
        case TAC_OR: return "||";
        case TAC_NOT: return "!";
        default: return "?";
    }
}


/* ==========================================================================
 * FORMATTING
 * --------------------------------------------------------------------------
 * One function renders a TAC instruction as text.  Everything that displays
 * or saves three-address code goes through it, so the listing you read on
 * screen and the listing saved to the .tac file can never drift apart.
 * ========================================================================*/
void formatTAC(const TACInstr* i, char* buf, size_t n) {
    if (!i) { snprintf(buf, n, "(null)"); return; }
    switch (i->op) {
        case TAC_FUNC_BEGIN: snprintf(buf, n, "FUNC_BEGIN %s", i->result); break;
        case TAC_FUNC_END:   snprintf(buf, n, "FUNC_END %s", i->result); break;
        case TAC_PARAM:      snprintf(buf, n, "PARAM %s %s",
                                      i->arg1 ? i->arg1 : "int", i->result); break;
        case TAC_DECL:       snprintf(buf, n, "DECL %s %s",
                                      i->arg1 ? i->arg1 : "int", i->result); break;

        case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV:
        case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
        case TAC_EQ:  case TAC_NE:  case TAC_AND: case TAC_OR:
            snprintf(buf, n, "%s = %s %s %s", i->result, i->arg1,
                     getOpName(i->op), i->arg2); break;

        case TAC_NEG:        snprintf(buf, n, "%s = -%s", i->result, i->arg1); break;
        case TAC_NOT:        snprintf(buf, n, "%s = !%s", i->result, i->arg1); break;
        case TAC_ASSIGN:     snprintf(buf, n, "%s = %s", i->result, i->arg1); break;
        case TAC_PRINT:      snprintf(buf, n, "PRINT %s", i->arg1); break;
        case TAC_ARG:        snprintf(buf, n, "ARG %s", i->arg1); break;
        case TAC_CALL:       snprintf(buf, n, "%s = CALL %s, %s",
                                      i->result, i->arg1, i->arg2); break;
        case TAC_RETURN:
            if (i->arg1) snprintf(buf, n, "RETURN %s", i->arg1);
            else         snprintf(buf, n, "RETURN");
            break;
        case TAC_LABEL:      snprintf(buf, n, "%s:", i->result); break;
        case TAC_GOTO:       snprintf(buf, n, "GOTO %s", i->arg1); break;
        case TAC_IF_FALSE:   snprintf(buf, n, "IF_FALSE %s GOTO %s", i->arg1, i->arg2); break;
        case TAC_IF_TRUE:    snprintf(buf, n, "IF_TRUE %s GOTO %s", i->arg1, i->arg2); break;
        case TAC_ARRAY_DECL: snprintf(buf, n, "ARRAY_DECL %s[%s]", i->result, i->arg1); break;
        case TAC_ARRAY_LOAD: snprintf(buf, n, "%s = %s[%s]", i->result, i->arg1, i->arg2); break;
        case TAC_ARRAY_STORE:snprintf(buf, n, "%s[%s] = %s", i->arg1, i->arg2, i->result); break;
        default:             snprintf(buf, n, "UNKNOWN"); break;
    }
}

/* Count instructions in a list — the crudest possible code-size metric, and
 * the one Topic 4 uses to quantify what optimization bought us. */
int countTAC(const TACList* list) {
    int n = 0;
    for (TACInstr* c = list->head; c; c = c->next) n++;
    return n;
}

static void dumpList(const TACList* list, const char* title) {
    char buf[256];
    trace("%s\n", title);
    trace("─────────────────────────────────────────────\n");
    int line = 1;
    for (TACInstr* c = list->head; c; c = c->next) {
        formatTAC(c, buf, sizeof buf);
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END)
            trace("%3d: %s\n", line++, buf);
        else
            trace("%3d:     %s\n", line++, buf);
    }
    trace("─────────────────────────────────────────────\n");
    trace("     %d instructions\n\n", countTAC(list));
}

void printTAC(void)          { dumpList(&tacList,       "UNOPTIMIZED THREE-ADDRESS CODE"); }
void printOptimizedTAC(void) { dumpList(&optimizedList, "OPTIMIZED THREE-ADDRESS CODE"); }

TACList* getOptimizedTAC(void) { return &optimizedList; }
TACList* getUnoptimizedTAC(void) { return &tacList; }

static void saveList(const TACList* list, const char* filename, const char* banner) {
    FILE* f = fopen(filename, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", filename); return; }
    char buf[256];
    fprintf(f, "; %s\n", banner);
    fprintf(f, "; Generated by the CST-405 mini compiler\n;\n");
    int line = 1;
    for (TACInstr* c = list->head; c; c = c->next) {
        formatTAC(c, buf, sizeof buf);
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END)
            fprintf(f, "%3d: %s\n", line++, buf);
        else
            fprintf(f, "%3d:     %s\n", line++, buf);
    }
    fprintf(f, ";\n; %d instructions\n", countTAC(list));
    fclose(f);
}

void saveTACToFile(const char* filename)          { saveList(&tacList,       filename, "Unoptimized three-address code"); }
void saveOptimizedTACToFile(const char* filename) { saveList(&optimizedList, filename, "Optimized three-address code"); }

/* ==========================================================================
 * PHASE 5 — OPTIMIZATION
 * --------------------------------------------------------------------------
 * The optimizer rewrites the TAC list into a shorter, cheaper list that
 * computes the same thing.  It runs the same set of transformations REPEATEDLY
 * until a pass changes nothing, because optimizations feed each other:
 *
 *      t0 = 2 * 3        constant folding  ->  t0 = 6
 *      x  = t0                                  x  = 6      (copy propagation)
 *      y  = x + 0                               y  = 6      (algebraic + folding)
 *      t0 = ...                                 (t0 now dead -> removed)
 *
 * No single pass finds all of that; three passes do.  Reaching a fixed point
 * is the standard way real optimizers are structured.
 *
 * Transformations implemented, in the order they are applied within a pass:
 *
 *   1. Algebraic simplification   x+0, x-0, x*1, x*0, x/1
 *   2. Constant folding           evaluate operations on two literals
 *   3. Constant propagation       replace a variable by a constant known to
 *                                 hold it at that point
 *   4. Copy propagation           replace t1 by x after "t1 = x"
 *   5. Dead code elimination      drop assignments to names never read again
 *   6. Unreachable code removal   drop code between GOTO and the next label
 *   7. Branch simplification      constant conditions become GOTO or nothing
 *   8. Loop-safe constant facts   (Project 4) a local assigned exactly once,
 *                                 to a constant, in the function's entry
 *                                 block keeps that constant ACROSS labels,
 *                                 so it reaches the inside of loops
 *   9. Jump-to-next removal       (Project 4) a GOTO or IF_FALSE/IF_TRUE whose
 *                                 target is the very next label does nothing
 *  11. Redundant store removal    (Project 4) "x = 5" when x is already
 *                                 known to hold 5 at that point
 *  10. Copy coalescing (peephole) (Project 4) "t = a op b ; v = t" becomes
 *                                 "v = a op b" when t is dead afterwards
 *
 * SAFETY RULES the passes obey, and that students must not break when they
 * extend this file:
 *   • A LABEL ends a basic block: forget everything known about values,
 *     because control can arrive here from anywhere.
 *   • A CALL may modify globals and arrays: forget facts about non-temporaries.
 *   • Never eliminate a store to an array or a global as "dead"; we do not
 *     track those precisely enough to prove it.
 * ========================================================================*/

#define MAX_FACTS 256

typedef struct {
    char* name;      /* The variable or temporary the fact is about */
    char* value;     /* The constant, or the name it is a copy of   */
    int   isConst;   /* 1 = value is a literal, 0 = value is a name */
} Fact;

static Fact  facts[MAX_FACTS];
static int   factCount = 0;
static int   changesThisPass = 0;

/* LOOP-SAFE CONSTANTS (technique 8, added in Project 4)
 * Clearing every fact at a LABEL is what keeps propagation sound around
 * loops, but it also throws away facts that cannot have changed.  Example
 * (tests/t4_10_bench_hotloop.cm):
 *
 *      k = 0                      <- entry block, k's ONLY assignment
 *  L0: ...  t0 = i + k  ...       <- inside the loop: k is still 0
 *
 * A local scalar that is assigned exactly once in its function, with a
 * constant, in the ENTRY block (before the first label or branch), holds
 * that constant at every later point: the entry block runs first and runs
 * once, nothing else writes the name, and a call cannot write it (scalars
 * are never passed by reference).  Those facts are "pinned": re-installed
 * after every label and call.  Globals, parameters and arrays never
 * qualify. */
static Fact  pinned[MAX_FACTS];
static int   pinnedCount = 0;

/* Does this instruction (re)define its `result` field? */
static int definesResult(TACOp op) {
    switch (op) {
        case TAC_ARRAY_STORE:                       /* result is the value READ */
        case TAC_LABEL: case TAC_FUNC_BEGIN: case TAC_FUNC_END:
        case TAC_DECL:  case TAC_ARRAY_DECL: case TAC_PARAM:
        case TAC_GOTO:  case TAC_IF_FALSE:   case TAC_IF_TRUE:
        case TAC_PRINT: case TAC_ARG:        case TAC_RETURN:
            return 0;
        default:
            return 1;
    }
}

static int isConstantNumber(const char* s);          /* defined below */

/* Called at FUNC_BEGIN: find this function's pinnable constants. */
static void computePinned(TACInstr* begin) {
    pinnedCount = 0;
    int inEntry = 1;
    for (TACInstr* c = begin->next; c && c->op != TAC_FUNC_END; c = c->next) {
        if (c->op == TAC_LABEL || c->op == TAC_GOTO ||
            c->op == TAC_IF_FALSE || c->op == TAC_IF_TRUE) inEntry = 0;
        if (c->op != TAC_DECL || pinnedCount >= MAX_FACTS) continue;

        const char* name = c->result;          /* a local scalar */
        TACInstr* def = NULL; int defs = 0, defInEntry = 0, entry = 1;
        for (TACInstr* d = begin->next; d && d->op != TAC_FUNC_END; d = d->next) {
            if (d->op == TAC_LABEL || d->op == TAC_GOTO ||
                d->op == TAC_IF_FALSE || d->op == TAC_IF_TRUE) entry = 0;
            if (definesResult(d->op) && d->result && strcmp(d->result, name) == 0) {
                defs++; def = d; defInEntry = entry;
            }
        }
        if (defs == 1 && defInEntry && def->op == TAC_ASSIGN && isConstantNumber(def->arg1)) {
            pinned[pinnedCount].name    = (char*)name;
            pinned[pinnedCount].value   = def->arg1;
            pinned[pinnedCount].isConst = 1;
            pinnedCount++;
        }
    }
    (void)inEntry;
}

/* Optimization bookkeeping, reported to the user and used by Topic 4 to
 * quantify the gain from each technique. */
static OptStats optStats;

OptStats getOptStats(void) { return optStats; }

static void clearFacts(void) { factCount = 0; }

/* Re-install every pinned constant whose (single) definition is already
 * behind us.  All pinned definitions are in the entry block, and labels and
 * calls come after it, so "already behind us" is tracked by pinnedLive. */
static int pinnedLive[MAX_FACTS];

static int isPinned(const char* name) {
    for (int i = 0; i < pinnedCount; i++)
        if (pinnedLive[i] && strcmp(pinned[i].name, name) == 0) return 1;
    return 0;
}
static void reseedPinned(void) {
    for (int i = 0; i < pinnedCount && factCount < MAX_FACTS; i++)
        if (pinnedLive[i]) facts[factCount++] = pinned[i];
}

static void dropFactsAbout(const char* name) {
    for (int i = 0; i < factCount; i++) {
        if (strcmp(facts[i].name, name) == 0 ||
            (!facts[i].isConst && strcmp(facts[i].value, name) == 0)) {
            facts[i] = facts[--factCount];
            i--;
        }
    }
}

/* A call can change any global or array element, so only facts about
 * compiler temporaries survive it -- and of those, only the ones that do not
 * say "this temporary is a COPY of a variable": after  t0 = g ; CALL f  the
 * temporary still holds the OLD g, so rewriting t0 back into g would read the
 * value f left behind.  (Restored from our Project 3 optimizer; it is what
 * keeps the left-to-right snapshots in place.) */
static void dropNonTempFacts(void) {
    for (int i = 0; i < factCount; i++) {
        const char* n = facts[i].name;
        int temp = (n[0] == 't' && n[1] >= '0' && n[1] <= '9');
        int copiesVariable = !facts[i].isConst;
        if (!temp || copiesVariable) { facts[i] = facts[--factCount]; i--; }
    }
}

static void recordFact(const char* name, const char* value, int isConst) {
    dropFactsAbout(name);
    if (factCount >= MAX_FACTS) return;
    facts[factCount].name    = (char*)name;
    facts[factCount].value   = (char*)value;
    facts[factCount].isConst = isConst;
    factCount++;
}

static const char* lookupFact(const char* name, int wantConst) {
    if (!name) return NULL;
    for (int i = 0; i < factCount; i++)
        if (strcmp(facts[i].name, name) == 0 && (!wantConst || facts[i].isConst))
            return facts[i].value;
    return NULL;
}

/* True for opcodes that only compute a value into `result` and have no other
 * effect.  Only these are candidates for dead-code elimination: a CALL may
 * print or modify globals, a STORE writes memory, a branch changes control. */
static int mnemonicIsPure(TACOp op) {
    switch (op) {
        case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV: case TAC_NEG:
        case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
        case TAC_EQ:  case TAC_NE:  case TAC_AND: case TAC_OR: case TAC_NOT:
        case TAC_ARRAY_LOAD:
            return 1;
        default:
            return 0;
    }
}

/* Is this operand a literal integer? */
static int isConstantNumber(const char* s) {
    if (!s || !*s) return 0;
    int i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Evaluate an operation whose operands are both literals. */
static char* foldConstants(TACOp op, const char* a1, const char* a2, int* folded) {
    *folded = 0;
    if (!isConstantNumber(a1) || !isConstantNumber(a2)) return NULL;
    long a = atol(a1), b = atol(a2), r;
    switch (op) {
        case TAC_ADD: r = a + b; break;
        case TAC_SUB: r = a - b; break;
        case TAC_MUL: r = a * b; break;
        case TAC_DIV: if (b == 0) return NULL;   /* leave division by zero
                                                  * alone: folding it would
                                                  * change a run-time trap
                                                  * into a compile-time one */
                      r = a / b; break;
        case TAC_LT:  r = a <  b; break;
        case TAC_GT:  r = a >  b; break;
        case TAC_LE:  r = a <= b; break;
        case TAC_GE:  r = a >= b; break;
        case TAC_EQ:  r = a == b; break;
        case TAC_NE:  r = a != b; break;
        case TAC_AND: r = (a != 0) && (b != 0); break;
        case TAC_OR:  r = (a != 0) || (b != 0); break;
        default: return NULL;
    }
    char* s = malloc(24);
    snprintf(s, 24, "%ld", r);
    *folded = 1;
    return s;
}

/* Does `name` get read anywhere at or after instruction `from`?
 * A conservative liveness test: it stops being conservative only for
 * compiler temporaries, which is exactly where dead code piles up. */
static int isReadLater(TACInstr* from, const char* name) {
    /* Project 4 sharpening.  The temporary allocator REUSES names (t0 is
     * handed out again as soon as it is freed), so the old question "is t0
     * read ANYWHERE later?" was almost always yes, and dead copies such as
     * t1 = i  survived inside loops.  The sharper test rests on one
     * invariant of our TAC generator:
     *
     *   A TEMPORARY NEVER LIVES ACROSS A BASIC-BLOCK BOUNDARY.
     *
     * Every temporary is created and consumed inside one statement; the
     * loop condition's temporary is consumed by the IF_FALSE right after it;
     * and propagation only substitutes within a block (facts are cleared at
     * labels; pinned facts are constants, never temporaries).  So scanning
     * forward, a temporary is dead as soon as we reach
     *   - an instruction that redefines it without reading it, or
     *   - a label, jump or branch (after checking whether that very
     *     instruction reads it), or the end of the function.
     * isReadLater is only ever asked about temporaries (see technique 5).
     * codegen.c relies on the same invariant to skip storing dead
     * temporaries at labels and branches. */
    for (TACInstr* c = from; c; c = c->next) {
        if (c->op == TAC_FUNC_END || c->op == TAC_LABEL) return 0;
        if (c->arg1 && strcmp(c->arg1, name) == 0) return 1;
        if (c->arg2 && strcmp(c->arg2, name) == 0) return 1;
        /* ARRAY_STORE reads its `result` field (the value being stored) */
        if (c->op == TAC_ARRAY_STORE && c->result && strcmp(c->result, name) == 0) return 1;
        if (c->op == TAC_GOTO || c->op == TAC_IF_FALSE || c->op == TAC_IF_TRUE ||
            c->op == TAC_RETURN) return 0;
        if (definesResult(c->op) && c->result && strcmp(c->result, name) == 0)
            return 0;                         /* overwritten before any read */
    }
    return 0;
}

/* Copy a TAC list so a pass can rewrite it without destroying the original. */
static TACList copyList(const TACList* src) {
    TACList d = { NULL, NULL, src->tempCount, src->labelCount };
    for (TACInstr* c = src->head; c; c = c->next) {
        TACInstr* n = createTAC(c->op, c->arg1, c->arg2, c->result);
        if (!d.head) d.head = d.tail = n;
        else { d.tail->next = n; d.tail = n; }
    }
    return d;
}

/* -------------------------------------------------------------------------
 * One optimization pass over `in`, producing `out`.
 * Returns the number of changes made.
 * -----------------------------------------------------------------------*/
static TACList optimizePass(TACList* in) {
    TACList out = { NULL, NULL, in->tempCount, in->labelCount };
    clearFacts();
    int unreachable = 0;

    for (TACInstr* c = in->head; c; c = c->next) {

        /* --- 6. UNREACHABLE CODE ------------------------------------------
         * Everything between an unconditional GOTO and the next label can
         * never execute. */
        if (unreachable) {
            if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END) {
                unreachable = 0;
            } else {
                optStats.unreachable++; changesThisPass++;
                continue;
            }
        }

        /* --- block boundaries: facts stop being valid --------------------- */
        if (c->op == TAC_FUNC_BEGIN) {
            computePinned(c);
            memset(pinnedLive, 0, sizeof pinnedLive);
        }
        if (c->op == TAC_FUNC_END) pinnedCount = 0;
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END) {
            clearFacts();
            reseedPinned();                    /* technique 8 */
        }
        if (c->op == TAC_CALL) { dropNonTempFacts(); reseedPinned(); }

        char* a1 = c->arg1;
        char* a2 = c->arg2;

        /* --- 3/4. CONSTANT AND COPY PROPAGATION --------------------------
         * Substitute into the operands (never into `result`, which is being
         * defined rather than read). ARRAY_STORE is the exception: its
         * `result` field is the value being stored, so it is a read. */
        int isRead1 = (c->op != TAC_LABEL && c->op != TAC_GOTO &&
                       c->op != TAC_FUNC_BEGIN && c->op != TAC_FUNC_END &&
                       c->op != TAC_DECL && c->op != TAC_PARAM &&
                       c->op != TAC_ARRAY_DECL && c->op != TAC_CALL);
        if (isRead1 && a1 && !isConstantNumber(a1)) {
            const char* k = lookupFact(a1, 1);
            if (k) { a1 = (char*)k; optStats.constProp++; changesThisPass++;
                     if (isPinned(c->arg1)) optStats.loopConst++; }
            else {
                const char* cp = lookupFact(a1, 0);
                if (cp) { a1 = (char*)cp; optStats.copyProp++; changesThisPass++; }
            }
        }
        if (a2 && !isConstantNumber(a2) && c->op != TAC_IF_FALSE && c->op != TAC_IF_TRUE
               && c->op != TAC_CALL) {
            const char* k = lookupFact(a2, 1);
            if (k) { a2 = (char*)k; optStats.constProp++; changesThisPass++;
                     if (isPinned(c->arg2)) optStats.loopConst++; }
            else {
                const char* cp = lookupFact(a2, 0);
                if (cp) { a2 = (char*)cp; optStats.copyProp++; changesThisPass++; }
            }
        }

        TACInstr* emit = NULL;

        /* --- 1. ALGEBRAIC SIMPLIFICATION ---------------------------------
         * Identities that hold for every integer value, so they are safe
         * without knowing anything about the operands. */
        if (!emit && (c->op == TAC_ADD || c->op == TAC_SUB)) {
            if (a2 && isConstantNumber(a2) && atol(a2) == 0) {           /* x ± 0 */
                emit = createTAC(TAC_ASSIGN, a1, NULL, c->result);
                optStats.algebraic++; changesThisPass++;
            } else if (c->op == TAC_ADD && a1 && isConstantNumber(a1) && atol(a1) == 0) {
                emit = createTAC(TAC_ASSIGN, a2, NULL, c->result);       /* 0 + x */
                optStats.algebraic++; changesThisPass++;
            }
        }
        if (!emit && c->op == TAC_MUL) {
            if ((a2 && isConstantNumber(a2) && atol(a2) == 0) ||
                (a1 && isConstantNumber(a1) && atol(a1) == 0)) {         /* x * 0 */
                emit = createTAC(TAC_ASSIGN, "0", NULL, c->result);
                optStats.algebraic++; changesThisPass++;
            } else if (a2 && isConstantNumber(a2) && atol(a2) == 1) {    /* x * 1 */
                emit = createTAC(TAC_ASSIGN, a1, NULL, c->result);
                optStats.algebraic++; changesThisPass++;
            } else if (a1 && isConstantNumber(a1) && atol(a1) == 1) {    /* 1 * x */
                emit = createTAC(TAC_ASSIGN, a2, NULL, c->result);
                optStats.algebraic++; changesThisPass++;
            }
        }
        if (!emit && c->op == TAC_DIV && a2 && isConstantNumber(a2) && atol(a2) == 1) {
            emit = createTAC(TAC_ASSIGN, a1, NULL, c->result);           /* x / 1 */
            optStats.algebraic++; changesThisPass++;
        }

        /* --- 2. CONSTANT FOLDING ------------------------------------------ */
        if (!emit) {
            int folded = 0;
            char* v = foldConstants(c->op, a1, a2, &folded);
            if (folded) {
                emit = createTAC(TAC_ASSIGN, v, NULL, c->result);
                optStats.constFold++; changesThisPass++;
                free(v);
            }
        }
        if (!emit && c->op == TAC_NEG && isConstantNumber(a1)) {
            char v[24]; snprintf(v, sizeof v, "%ld", -atol(a1));
            emit = createTAC(TAC_ASSIGN, v, NULL, c->result);
            optStats.constFold++; changesThisPass++;
        }
        if (!emit && c->op == TAC_NOT && isConstantNumber(a1)) {
            emit = createTAC(TAC_ASSIGN, atol(a1) == 0 ? "1" : "0", NULL, c->result);
            optStats.constFold++; changesThisPass++;
        }

        /* --- 7. BRANCH SIMPLIFICATION --------------------------------------
         * Folding and propagation may have turned the condition into a
         * literal.  A branch that always goes is an unconditional GOTO; one
         * that never goes is deleted.  A new GOTO makes the code after it
         * unreachable, which the NEXT pass removes: a small demonstration of
         * why the optimizer must run to a fixed point. */
        if (!emit && (c->op == TAC_IF_FALSE || c->op == TAC_IF_TRUE) && isConstantNumber(a1)) {
            int takes = (c->op == TAC_IF_FALSE) ? (atol(a1) == 0) : (atol(a1) != 0);
            optStats.branch++; changesThisPass++;
            if (!takes) continue;                       /* never jumps: delete */
            emit = createTAC(TAC_GOTO, a2, NULL, NULL); /* always jumps */
        }


        /* Nothing rewrote it: keep the instruction, with propagated operands. */
        if (!emit) emit = createTAC(c->op, a1, a2, c->result);

        /* --- 11. REDUNDANT STORE ------------------------------------------
         * "x = 5" where the facts already say x == 5 here changes nothing.
         * (Facts are block-local and dropped at calls, so this is safe for
         * globals as well as locals.)  Checked BEFORE this instruction
         * records its own fact, or every constant store would look
         * redundant. */
        if (emit->op == TAC_ASSIGN && isConstantNumber(emit->arg1) && emit->result) {
            const char* known = lookupFact(emit->result, 1);
            if (known && strcmp(known, emit->arg1) == 0) {
                optStats.redundantStore++; changesThisPass++;
                continue;
            }
        }

        /* --- 5. DEAD CODE ELIMINATION ------------------------------------
         * An assignment to a TEMPORARY that nothing reads afterward can go.
         * Only temporaries: a store to a user variable or a global might be
         * observed by code this simple analysis cannot see. */
        if (emit->op == TAC_ASSIGN || (mnemonicIsPure(emit->op))) {
            const char* d = emit->result;
            if (d && d[0] == 't' && d[1] >= '0' && d[1] <= '9' && !isReadLater(c->next, d)) {
                optStats.deadCode++; changesThisPass++;
                continue;
            }
        }

        /* --- 10. COPY COALESCING (peephole) -------------------------------
         * Every assignment statement  v = a op b  is generated as
         *     t = a op b
         *     v = t
         * If the very next instruction copies the temporary into a variable
         * and the temporary is not read afterwards, compute straight into
         * the variable.  One TAC instruction (and one MIPS move) per
         * statement disappears; inside a loop that is one per iteration. */
        if (mnemonicIsPure(emit->op) && emit->result && emit->result[0] == 't' &&
            emit->result[1] >= '0' && emit->result[1] <= '9' &&
            c->next && c->next->op == TAC_ASSIGN && c->next->arg1 &&
            strcmp(c->next->arg1, c->result) == 0 &&
            !isReadLater(c->next->next, c->result)) {
            free(emit->result);
            emit->result = strdup(c->next->result);
            optStats.coalesce++; changesThisPass++;
            c = c->next;                       /* the copy is now absorbed */
        }

        /* --- record what we now know for later instructions ---------------- */
        switch (emit->op) {
            case TAC_ASSIGN:
                if (isConstantNumber(emit->arg1)) recordFact(emit->result, emit->arg1, 1);
                else                              recordFact(emit->result, emit->arg1, 0);
                for (int p = 0; p < pinnedCount; p++)          /* technique 8 */
                    if (strcmp(pinned[p].name, emit->result) == 0) pinnedLive[p] = 1;
                break;
            case TAC_ARRAY_STORE:
                /* We do not track array contents; nothing to record. */
                break;
            default:
                if (emit->result) dropFactsAbout(emit->result);
                break;
        }

        /* --- 9. JUMP TO THE NEXT INSTRUCTION -----------------------------
         * "GOTO L" immediately followed by "L:" does nothing but cost a jump
         * (and, in codegen, a register flush).  Branch simplification can
         * leave exactly this shape behind, e.g. a loop whose body vanished. */
        if (emit->op == TAC_GOTO && c->next && c->next->op == TAC_LABEL &&
            strcmp(c->next->result, emit->arg1) == 0) {
            optStats.jumpNext++; changesThisPass++;
            continue;
        }
        /* The conditional form: both outcomes land on the same instruction,
         * so the test is pointless (and its condition temporary becomes dead,
         * which dead-code elimination removes in the next pass).  This is
         * what is left of a loop whose body was nothing but `break`. */
        if ((emit->op == TAC_IF_FALSE || emit->op == TAC_IF_TRUE) && c->next &&
            c->next->op == TAC_LABEL && strcmp(c->next->result, emit->arg2) == 0) {
            optStats.jumpNext++; changesThisPass++;
            continue;
        }

        if (emit->op == TAC_GOTO) unreachable = 1;

        if (!out.head) out.head = out.tail = emit;
        else { out.tail->next = emit; out.tail = emit; }
    }
    return out;
}

/* -O0: hand the back end an untouched copy of the TAC.  The scorecard
 * reports 0 passes and no change, which makes the benchmark comparison
 * (make bench) an honest optimized-versus-unoptimized measurement. */
void skipOptimization(void) {
    memset(&optStats, 0, sizeof optStats);
    optimizedList = copyList(&tacList);
    optStats.instructionsBefore = countTAC(&tacList);
    optStats.instructionsAfter  = optStats.instructionsBefore;
}

void optimizeTAC(void) {
    memset(&optStats, 0, sizeof optStats);
    optStats.instructionsBefore = countTAC(&tacList);

    TACList work = copyList(&tacList);

    int pass = 0;
    trace("Running optimization passes until no further change:\n");
    do {
        changesThisPass = 0;
        TACList next = optimizePass(&work);
        work = next;
        pass++;
        trace("  pass %d: %3d change(s), %3d instructions\n",
               pass, changesThisPass, countTAC(&work));
    } while (changesThisPass > 0 && pass < 20);

    optimizedList = work;
    optStats.passes = pass;
    optStats.instructionsAfter = countTAC(&optimizedList);

    int removed = optStats.instructionsBefore - optStats.instructionsAfter;
    double pct = optStats.instructionsBefore
               ? (100.0 * removed / optStats.instructionsBefore) : 0.0;

    trace("\n  Technique                     Applications\n");
    trace("  ─────────────────────────────────────────\n");
    trace("  Algebraic simplification      %6d\n", optStats.algebraic);
    trace("  Constant folding              %6d\n", optStats.constFold);
    trace("  Constant propagation          %6d\n", optStats.constProp);
    trace("  Copy propagation              %6d\n", optStats.copyProp);
    trace("  Dead code elimination         %6d\n", optStats.deadCode);
    trace("  Unreachable code removal      %6d\n", optStats.unreachable);
    trace("  Branch simplification         %6d\n", optStats.branch);
    trace("  Loop-safe constants (pinned)  %6d   (also counted in const prop)\n", optStats.loopConst);
    trace("  Jump-to-next removal          %6d\n", optStats.jumpNext);
    trace("  Copy coalescing (peephole)    %6d\n", optStats.coalesce);
    trace("  Redundant store removal       %6d\n", optStats.redundantStore);
    trace("  ─────────────────────────────────────────\n");
    trace("  TAC instructions   %d -> %d  (%d removed, %.1f%% smaller)\n\n",
           optStats.instructionsBefore, optStats.instructionsAfter, removed, pct);
}
