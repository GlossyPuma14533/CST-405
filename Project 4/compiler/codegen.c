/* =========================================================================
 * CST-405  ·  TOPIC 4  ·  Compiling Loops
 * FILE: codegen.c   —   Phase 6 — MIPS code generation
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                                                  ^^^^^^^  this file
 *
 * WHAT IS NEW IN TOPIC 4
 *   • Branch and label emission; registers are flushed at every control-flow join
 *
 * WHAT COMES NEXT
 *   Topic 5 adds decisions (if, if-else, switch) and the logical operators.
 *
 * STATUS
 *   Topic 4 label and branch emission is implemented.  Project 4 also
 *   restores register pinning and adds back-end peepholes (dead-store-free
 *   flushes, immediates, compare-and-branch fusion), all off under -O0/-O1.
 * ========================================================================= */

/* ============================================================================
 * PHASE 6 — MIPS CODE GENERATION
 * ----------------------------------------------------------------------------
 * Reads the optimized three-address code and writes MIPS assembly.
 *
 * The structure of this file mirrors the two problems named in codegen.h:
 *
 *   PART 1  Register cache   — allocate, spill, reload  ($t0-$t9)
 *   PART 2  Addressing       — turn a name into an address (global / local /
 *                              array element / array passed by reference)
 *   PART 3  Frame layout     — measure a function, emit prologue and epilogue
 *   PART 4  Instruction emit — one case per TAC opcode
 *
 * Read it in that order.  Nothing in PART 4 is complicated once PARTS 1-3
 * are understood; that is the whole point of separating them.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codegen.h"
#include "symtab.h"
#include "trace.h"

static FILE* out;                 /* The .s file being written              */
RegisterAllocator regAlloc;       /* The $t0-$t9 cache                      */

/* Argument names buffered between TAC_ARG instructions and the TAC_CALL that
 * consumes them.  A call always takes the LAST n buffered names, which is
 * what makes nested calls such as f(1, g(2)) come out right. */
static char* argBuf[16];
static int   argBufCount = 0;

static int paramIndex = 0;        /* Which $aN the next parameter arrives in */
static int frameSize  = 0;        /* Size of the frame being generated       */
static const char* currentFunc = NULL;

/* Back-end peepholes added in Project 4 (see PART 4 and flushRegisters).
 * main.c turns them off with -O0 so that the benchmark baseline is the
 * plain translation of the unoptimized TAC. */
int codegenOptimize = 1;

/* ==========================================================================
 * NAME MANGLING
 * --------------------------------------------------------------------------
 * Source identifiers and assembler labels share one namespace, and the
 * assembler's namespace already contains every instruction mnemonic.  A user
 * who writes
 *         int add(int a, int b) { return a + b; }
 * would otherwise make us emit the label `add:` — and SPIM would reject the
 * file, pointing at a line the programmer never wrote.
 *
 * The fix every real compiler uses is to keep the two namespaces apart by
 * decorating names on the way out:
 *
 *      function add   ->   fn_add          global counter  ->  g_counter
 *      function main  ->   main            (the entry point must keep its name)
 *
 * Two static buffers are enough because no call site needs more than two
 * mangled names alive at once.
 * ========================================================================*/
static const char* funcLabel(const char* name) {
    static char buf[2][MAX_VAR_NAME + 8];
    static int which = 0;
    if (strcmp(name, "main") == 0) return "main";   /* the entry point */
    which ^= 1;
    snprintf(buf[which], sizeof buf[which], "fn_%s", name);
    return buf[which];
}

static const char* dataLabel(const char* name) {
    static char buf[2][MAX_VAR_NAME + 8];
    static int which = 0;
    which ^= 1;
    snprintf(buf[which], sizeof buf[which], "g_%s", name);
    return buf[which];
}

/* ==========================================================================
 * Small predicates used throughout
 * ========================================================================*/

/* Is this TAC operand a literal integer rather than a name? */
static int isConstant(const char* s) {
    if (!s || !*s) return 0;
    int i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Is this a compiler-generated temporary (t0, t1, ...)?  Temporaries are
 * reserved identifiers — semantic.c rejects user variables of this shape —
 * so the test is safe. */
static int isTemp(const char* s) {
    if (!s || s[0] != 't' || !s[1]) return 0;
    for (int i = 1; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* ==========================================================================
 * PART 1 — REGISTER CACHE
 * --------------------------------------------------------------------------
 * Ten registers stand in for an unbounded set of TAC names.  The rules:
 *
 *   ensureReg(name)  the register must hold the CURRENT value of `name`
 *                    (load it from memory if it is not already cached)
 *   defReg(name)     the register is about to be OVERWRITTEN with a new
 *                    value of `name` (no load needed; mark it dirty)
 *   scratchReg()     an anonymous register for address arithmetic
 *
 * When no register is free the least-recently-used one is evicted.  Eviction
 * writes the value back first if it is dirty AND the name has a memory home.
 * Literals and scratch values have no home, so evicting them is free.
 * ========================================================================*/

static void emitStoreHome(int reg, const char* name);
static void emitLoadHome(int reg, const char* name);

void initRegAlloc(void) {
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        regAlloc.regs[i].varName[0] = '\0';
        regAlloc.regs[i].isDirty    = 0;
        regAlloc.regs[i].inUse      = 0;
        regAlloc.regs[i].hasHome    = 0;
        regAlloc.regs[i].lastUsed   = 0;
    }
    regAlloc.timestamp  = 0;
    regAlloc.spillCount = 0;
    regAlloc.loadCount  = 0;
}

/* Which register currently caches `name`?  -1 if none. */
static int findVarReg(const char* name) {
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        if (regAlloc.regs[i].inUse && strcmp(regAlloc.regs[i].varName, name) == 0) {
            regAlloc.regs[i].lastUsed = ++regAlloc.timestamp;
            return i;
        }
    }
    return -1;
}

/* PINNED REGISTERS (restored from our Project 3 code generator).
 * One TAC instruction may need several registers at once (two operands, a
 * destination, an array base and offset).  A register handed out for the
 * CURRENT instruction is pinned, so the LRU allocator can never evict it to
 * satisfy the next request of that same instruction.  Without this, with
 * all ten $t registers busy,  arr[1] = 5  loaded 5 into a register and then
 * reused that same register for the array's base address, storing the
 * address instead of 5.  Found by differential fuzzing in Project 4 (random
 * loop programs, -O0 vs -O2 vs a reference interpreter); it was present in
 * the Topic 4 starting code at every optimization level. */
static int pinned[NUM_TEMP_REGS];
static void unpinAll(void) { for (int i = 0; i < NUM_TEMP_REGS; i++) pinned[i] = 0; }

/* Least-recently-used register — the one whose value we can most afford
 * to lose.  A real compiler would use liveness information here; LRU is a
 * good approximation and is easy to reason about in class.  Registers
 * pinned by the current instruction are never chosen. */
static int selectVictimReg(void) {
    int victim = -1, oldest = 0;
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        if (pinned[i]) continue;               /* in use by THIS instruction */
        /* Values with no memory home are the cheapest to discard */
        if (!regAlloc.regs[i].hasHome && regAlloc.regs[i].inUse) return i;
        if (victim == -1 || regAlloc.regs[i].lastUsed < oldest) {
            oldest = regAlloc.regs[i].lastUsed;
            victim = i;
        }
    }
    if (victim == -1) {                        /* cannot happen: <= 5 pins */
        fprintf(stderr, "codegen: every register is pinned\n");
        exit(1);
    }
    return victim;
}

/* Write a register back to its memory home and mark it clean. */
static void spillReg(int r) {
    if (regAlloc.regs[r].inUse && regAlloc.regs[r].isDirty && regAlloc.regs[r].hasHome) {
        fprintf(out, "    # spill: %s -> memory\n", regAlloc.regs[r].varName);
        emitStoreHome(r, regAlloc.regs[r].varName);
        regAlloc.spillCount++;
    }
    regAlloc.regs[r].isDirty = 0;
}

/* Grab a register for `name`.  `load` says whether the register must be
 * primed with the value already in memory. */
static int getReg(const char* name, int hasHome, int load) {
    int r = findVarReg(name);
    if (r != -1) { pinned[r] = 1; return r; }    /* Already cached          */

    for (r = 0; r < NUM_TEMP_REGS; r++)          /* A free register?        */
        if (!regAlloc.regs[r].inUse) break;

    if (r == NUM_TEMP_REGS) {                    /* None free — evict LRU   */
        r = selectVictimReg();
        spillReg(r);
    }

    strncpy(regAlloc.regs[r].varName, name, MAX_VAR_NAME - 1);
    regAlloc.regs[r].varName[MAX_VAR_NAME - 1] = '\0';
    regAlloc.regs[r].inUse    = 1;
    regAlloc.regs[r].isDirty  = 0;
    regAlloc.regs[r].hasHome  = hasHome;
    regAlloc.regs[r].lastUsed = ++regAlloc.timestamp;

    if (load) {
        emitLoadHome(r, name);
        regAlloc.loadCount++;
    }
    pinned[r] = 1;                               /* safe for this instruction */
    return r;
}

/* Register holding the current value of `name` (loading it if necessary). */
static int ensureReg(const char* name) { return getReg(name, 1, 1); }

/* Register that will RECEIVE a new value for `name`. */
static int defReg(const char* name) {
    int r = getReg(name, 1, 0);
    regAlloc.regs[r].isDirty = 1;
    return r;
}

/* Anonymous register for address arithmetic; never written back, because it
 * holds an address we computed rather than a value the program named. */
static int scratchCounter = 0;

/* Set by a comparison that has been fused into the branch after it. */
static TACInstr* fusedCompare = NULL;
static int scratchReg(void) {
    char n[MAX_VAR_NAME];
    snprintf(n, sizeof n, "#s%d", scratchCounter++);
    return getReg(n, 0, 0);
}
static void releaseReg(int r) {
    if (r < 0) return;
    pinned[r] = 0;
    regAlloc.regs[r].inUse      = 0;
    regAlloc.regs[r].isDirty    = 0;
    regAlloc.regs[r].hasHome    = 0;
    regAlloc.regs[r].varName[0] = '\0';
}

/* Materialise any TAC operand — literal or name — into a register. */
static int operandReg(const char* s) {
    if (isConstant(s)) {
        char n[MAX_VAR_NAME];
        snprintf(n, sizeof n, "#k%s", s);
        int r = findVarReg(n);
        if (r != -1) { pinned[r] = 1; return r; }
        r = getReg(n, 0, 0);
        fprintf(out, "    li   $t%d, %s\n", r, s);
        return r;
    }
    return ensureReg(s);
}

/* Write every dirty register back to memory, then forget all cached values.
 * Called before `jal` (the callee owns $t0-$t9) and before any label, since
 * control may reach a label from somewhere with a different cache state.
 *
 * WHAT MUST BE WRITTEN BACK (Project 4).  Writing back a value nobody will
 * read again is pure cost, and inside a loop it is paid every iteration:
 *   FLUSH_ALL      before a call: a temporary CAN be live across a call,
 *                  e.g.  (g + 0) + f()  holds the left operand meanwhile.
 *   FLUSH_NO_TEMPS at labels, jumps and branches: a temporary never lives
 *                  across a basic-block boundary (the invariant documented
 *                  at isReadLater in tac.c), so dead temporaries are dropped.
 *   FLUSH_GLOBALS  at return / end of function: locals and temporaries die
 *                  with the activation record; only globals must reach
 *                  memory.  (Restored from our Project 3 code generator.)
 * With -O0 every flush is FLUSH_ALL, the original behaviour. */
typedef enum { FLUSH_ALL, FLUSH_NO_TEMPS, FLUSH_GLOBALS } FlushMode;

static int mustWriteBack(int r, FlushMode mode) {
    if (!(regAlloc.regs[r].inUse && regAlloc.regs[r].isDirty && regAlloc.regs[r].hasHome))
        return 0;
    if (!codegenOptimize || mode == FLUSH_ALL) return 1;
    const char* n = regAlloc.regs[r].varName;
    if (mode == FLUSH_NO_TEMPS) return !isTemp(n);
    Symbol* sym = lookupSymbol(n);                       /* FLUSH_GLOBALS */
    return sym && sym->isGlobal;
}

static void flushRegistersMode(const char* why, FlushMode mode) {
    int any = 0;
    for (int i = 0; i < NUM_TEMP_REGS; i++)
        if (mustWriteBack(i, mode)) any = 1;
    if (any) fprintf(out, "    # write back live values (%s)\n", why);
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        if (mustWriteBack(i, mode)) spillReg(i);
        releaseReg(i);
    }
}

static void flushRegisters(const char* why) { flushRegistersMode(why, FLUSH_ALL); }

/* Before a CONDITIONAL branch (Project 4).  Memory must be current on the
 * taken path, so dirty values are written back exactly as for a label.  But
 * the FALL-THROUGH path is reachable only from here, and a branch changes no
 * register, so the cached values stay valid there: keep them (now clean)
 * instead of reloading them from memory one instruction later.  Dead
 * temporaries are still dropped.  With -O0: the original full flush. */
static void flushBeforeBranch(void) {
    if (!codegenOptimize) { flushRegisters("before conditional branch"); return; }
    int any = 0;
    for (int i = 0; i < NUM_TEMP_REGS; i++)
        if (mustWriteBack(i, FLUSH_NO_TEMPS)) any = 1;
    if (any) fprintf(out, "    # write back live values (before conditional branch)\n");
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        if (!regAlloc.regs[i].inUse) continue;
        if (mustWriteBack(i, FLUSH_NO_TEMPS)) spillReg(i);
        if (isTemp(regAlloc.regs[i].varName)) releaseReg(i);  /* dead past here */
    }
}

void printRegAllocStats(void) {
    printf("  Register spills (store to memory) : %d\n", regAlloc.spillCount);
    printf("  Register reloads (load from memory): %d\n", regAlloc.loadCount);
}

/* ==========================================================================
 * PART 2 — ADDRESSING
 * --------------------------------------------------------------------------
 * Turning a name into an address is the whole difference between a global,
 * a local, and an array parameter.  All three cases live here so that the
 * instruction cases in PART 4 never have to think about it.
 * ========================================================================*/

static void emitLoadHome(int reg, const char* name) {
    Symbol* s = lookupSymbol(name);
    if (!s) {
        fprintf(stderr, "codegen: '%s' has no storage assigned "
                        "(internal error in function '%s')\n",
                name, currentFunc ? currentFunc : "?");
        exit(1);
    }
    if (s->isGlobal) fprintf(out, "    lw   $t%d, %s\n", reg, dataLabel(s->name));
    else             fprintf(out, "    lw   $t%d, %d($sp)        # %s\n", reg, s->offset, s->name);
}

static void emitStoreHome(int reg, const char* name) {
    Symbol* s = lookupSymbol(name);
    if (!s) {
        fprintf(stderr, "codegen: '%s' has no storage assigned "
                        "(internal error in function '%s')\n",
                name, currentFunc ? currentFunc : "?");
        exit(1);
    }
    if (s->isGlobal) fprintf(out, "    sw   $t%d, %s\n", reg, dataLabel(s->name));
    else             fprintf(out, "    sw   $t%d, %d($sp)        # %s\n", reg, s->offset, s->name);
}

/* Put the ADDRESS of element 0 of `arrName` into a fresh scratch register.
 *
 *   global array      la   $t, name          (address of the .data label)
 *   local array       addi $t, $sp, offset   (the elements live in the frame)
 *   array parameter   lw   $t, offset($sp)   (the slot holds someone else's
 *                                             base address — pass by reference)
 */
static int arrayBaseReg(const char* arrName) {
    Symbol* s = lookupSymbol(arrName);
    if (!s || !s->isArray) {
        fprintf(stderr, "codegen: '%s' is not an array\n", arrName);
        exit(1);
    }
    int r = scratchReg();
    if (s->isGlobal)          fprintf(out, "    la   $t%d, %s              # base of %s\n", r, dataLabel(s->name), s->name);
    else if (s->isParamArray) fprintf(out, "    lw   $t%d, %d($sp)        # base of %s (by reference)\n", r, s->offset, s->name);
    else                      fprintf(out, "    addi $t%d, $sp, %d        # base of %s\n", r, s->offset, s->name);
    return r;
}

/* Address of arrName[indexOperand]: base + index*4 */
static int elementAddrReg(const char* arrName, const char* indexOperand) {
    int base = arrayBaseReg(arrName);
    int idx  = operandReg(indexOperand);
    int off  = scratchReg();
    fprintf(out, "    sll  $t%d, $t%d, 2         # index * 4 bytes\n", off, idx);
    fprintf(out, "    add  $t%d, $t%d, $t%d       # base + offset\n", base, base, off);
    releaseReg(off);
    return base;                 /* caller releases */
}

/* ==========================================================================
 * PART 3 — FRAME LAYOUT
 * --------------------------------------------------------------------------
 * Before emitting a single instruction of a function we walk its TAC once and
 * hand a memory home to every name it mentions.  Doing this up front is what
 * lets the register allocator spill anything at any moment.
 * ========================================================================*/

/* Give `name` a home if it does not have one yet (used for temporaries). */
static void reserveTemp(const char* name) {
    if (!name || isConstant(name)) return;
    if (!isTemp(name)) return;              /* declared names get homes from
                                             * their DECL/PARAM instruction */
    if (lookupSymbol(name)) return;
    addVar((char*)name, "int");
}

/* Walk one function's TAC (FUNC_BEGIN .. FUNC_END), populate the local symbol
 * table, and return the total frame size in bytes. */
static int layoutFrame(TACInstr* funcBegin) {
    initSymTab();

    for (TACInstr* c = funcBegin->next; c && c->op != TAC_FUNC_END; c = c->next) {
        switch (c->op) {
            case TAC_PARAM:
                /* arg1 carries "int[]" for arrays passed by reference */
                if (c->arg1 && strcmp(c->arg1, "int[]") == 0) addArrayParam(c->result);
                else                                          addVar(c->result, "int");
                break;
            case TAC_DECL:
                addVar(c->result, c->arg1 ? c->arg1 : "int");
                break;
            case TAC_ARRAY_DECL:
                addArray(c->result, atoi(c->arg1));
                break;
            default: break;
        }
        /* Any temporary mentioned anywhere needs a home too */
        reserveTemp(c->result);
        reserveTemp(c->arg1);
        reserveTemp(c->arg2);
    }

    /* locals + saved $ra + alignment padding, rounded up to a multiple of 8 */
    int size = getLocalBytes() + 8;
    if (size % 8) size += 8 - (size % 8);
    return size;
}

static void emitPrologue(const char* name, int size) {
    fprintf(out, "\n# ==== function %s ====\n", name);
    fprintf(out, "%s:\n", funcLabel(name));
    fprintf(out, "    addi $sp, $sp, -%d        # build activation record\n", size);
    fprintf(out, "    sw   $ra, %d($sp)        # save return address\n", size - 4);
}

static void emitEpilogue(const char* name, int size) {
    fprintf(out, "%s__epilogue:\n", funcLabel(name));
    fprintf(out, "    lw   $ra, %d($sp)        # restore return address\n", size - 4);
    fprintf(out, "    addi $sp, $sp, %d        # discard activation record\n", size);
    if (strcmp(name, "main") == 0) {
        fprintf(out, "    li   $v0, 10             # syscall 10 = exit\n");
        fprintf(out, "    syscall\n");
    } else {
        fprintf(out, "    jr   $ra                 # return to caller\n");
    }
}

/* ==========================================================================
 * PART 4 — INSTRUCTION SELECTION
 * ========================================================================*/

/* MIPS mnemonic for each arithmetic / relational / logical TAC opcode.
 * SPIM accepts the three-operand pseudo-instructions used here (mul, div,
 * slt, sgt, sle, sge, seq, sne), expanding them to real instructions. */
static const char* mnemonicFor(TACOp op) {
    switch (op) {
        case TAC_ADD: return "add";
        case TAC_SUB: return "sub";
        case TAC_MUL: return "mul";
        case TAC_DIV: return "div";
        /* MIPS "set on condition": each writes exactly 1 or 0 into the
         * destination, which is what our language says a comparison yields.
         * sgt/sle/sge/seq/sne are SPIM pseudo-instructions. */
        case TAC_LT:  return "slt";
        case TAC_GT:  return "sgt";
        case TAC_LE:  return "sle";
        case TAC_GE:  return "sge";
        case TAC_EQ:  return "seq";
        case TAC_NE:  return "sne";
        default:      return NULL;
    }
}

/* Emit the .data section: one entry per global declaration. */
static void emitDataSection(TACInstr* head) {
    initGlobalScope();
    fprintf(out, "# ============ CST-405 generated MIPS ============\n");
    fprintf(out, ".data\n");
    /* The newline string is emitted first and everything after it is word
     * aligned explicitly: .asciiz leaves the location counter on an odd byte,
     * and `lw`/`sw` on a misaligned address raises an address-error exception
     * in SPIM.  Forgetting .align here is a classic first bug. */
    fprintf(out, "__nl: .asciiz \"\\n\"\n");

    int inFunction = 0;
    for (TACInstr* c = head; c; c = c->next) {
        if (c->op == TAC_FUNC_BEGIN) { inFunction = 1; continue; }
        if (c->op == TAC_FUNC_END)   { inFunction = 0; continue; }
        if (inFunction) continue;                 /* only file-scope declarations */

        if (c->op == TAC_DECL) {
            addGlobalVar(c->result, c->arg1 ? c->arg1 : "int");
            fprintf(out, ".align 2\n%s: .word 0            # int %s\n",
                    dataLabel(c->result), c->result);
        } else if (c->op == TAC_ARRAY_DECL) {
            int n = atoi(c->arg1);
            addGlobalArray(c->result, n);
            fprintf(out, ".align 2\n%s: .space %d          # int %s[%d]\n",
                    dataLabel(c->result), n * 4, c->result, n);
        }
    }
    fprintf(out, "\n.text\n.globl main\n");
}

void generateMIPSFromTAC(const char* filename) {
    out = fopen(filename, "w");
    if (!out) { fprintf(stderr, "Cannot open output file %s\n", filename); exit(1); }

    initRegAlloc();

    TACList* tac = getOptimizedTAC();
    if (!tac || !tac->head) {
        fprintf(stderr, "Error: no TAC to translate\n");
        fclose(out);
        return;
    }

    /* Pass 1: globals -> .data */
    emitDataSection(tac->head);

    /* Pass 2: one function at a time */
    for (TACInstr* c = tac->head; c; c = c->next) {

        if (c->op != TAC_FUNC_BEGIN) continue;   /* file-scope decls already done */

        currentFunc = c->result;
        frameSize   = layoutFrame(c);
        trace("\n  Activation record for '%s': %d bytes\n", currentFunc, frameSize);
        printSymTab();
        paramIndex  = 0;
        argBufCount = 0;
        initRegAlloc();
        unpinAll();

        emitPrologue(currentFunc, frameSize);

        for (TACInstr* i = c->next; i && i->op != TAC_FUNC_END; i = i->next) {
            unpinAll();     /* a new instruction: the last one's registers may go */
            switch (i->op) {

            /* ---------- storage declarations: no code, just a comment ----- */
            case TAC_DECL: {
                Symbol* s = lookupSymbol(i->result);
                fprintf(out, "    # int %s  ->  %d($sp)\n", i->result, s ? s->offset : -1);
                break;
            }
            case TAC_ARRAY_DECL: {
                Symbol* s = lookupSymbol(i->result);
                fprintf(out, "    # int %s[%s]  ->  %d($sp)\n",
                        i->result, i->arg1, s ? s->offset : -1);
                break;
            }

            /* ---------- parameters arrive in $a0-$a3 --------------------- */
            case TAC_PARAM: {
                Symbol* s = lookupSymbol(i->result);
                if (paramIndex >= MAX_ARGS) {
                    fprintf(stderr,
                        "codegen: function '%s' declares more than %d parameters; "
                        "this compiler passes arguments in $a0-$a3 only.\n",
                        currentFunc, MAX_ARGS);
                    exit(1);
                }
                fprintf(out, "    sw   $a%d, %d($sp)        # parameter %s\n",
                        paramIndex, s->offset, i->result);
                paramIndex++;
                break;
            }

            /* ---------- arithmetic, comparison ---------------------------- */
            case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV:
            case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
            case TAC_EQ:  case TAC_NE:
            {
                /* PEEPHOLE: COMPARE-AND-BRANCH FUSION (Project 4).
                 * A loop test is generated as
                 *       t = i <= 100
                 *       IF_FALSE t GOTO Lend
                 * which costs a set-on-condition AND a branch on its result.
                 * When the very next instruction is that branch and t is read
                 * nowhere else, emit nothing here: the IF_FALSE case emits ONE
                 * branch on the inverted condition (bgt i, 100, Lend).  An
                 * immediate right operand is used directly. */
                if (codegenOptimize && i->op >= TAC_LT && i->op <= TAC_NE &&
                    i->next && (i->next->op == TAC_IF_FALSE || i->next->op == TAC_IF_TRUE) &&
                    i->next->arg1 && strcmp(i->next->arg1, i->result) == 0 && isTemp(i->result)) {
                    fusedCompare = i;
                    break;
                }

                /* PEEPHOLE: IMMEDIATE OPERANDS (Project 4).
                 *   x + 5   ->  addi d, x, 5      (no li for the constant)
                 *   x - 5   ->  addi d, x, -5
                 *   x * 8   ->  sll  d, x, 3      (strength reduction)
                 * Constants on the left of + and * are swapped to the right
                 * (both are commutative). */
                if (codegenOptimize && (i->op == TAC_ADD || i->op == TAC_SUB || i->op == TAC_MUL)) {
                    const char* x = i->arg1; const char* k = i->arg2;
                    if (i->op != TAC_SUB && isConstant(x) && !isConstant(k)) { x = i->arg2; k = i->arg1; }
                    if (!isConstant(x) && isConstant(k)) {
                        long kv = atol(k);
                        if (i->op == TAC_SUB) kv = -kv;
                        if (i->op != TAC_MUL && kv >= -32768 && kv <= 32767) {
                            int a = operandReg(x);
                            int d = defReg(i->result);
                            fprintf(out, "    addi $t%d, $t%d, %ld        # %s = %s %s %s\n",
                                    d, a, kv, i->result, i->arg1,
                                    i->op == TAC_ADD ? "+" : "-", i->arg2);
                            break;
                        }
                        if (i->op == TAC_MUL && kv > 1 && (kv & (kv - 1)) == 0) {
                            int sh = 0; while ((1L << sh) < kv) sh++;
                            int a = operandReg(x);
                            int d = defReg(i->result);
                            fprintf(out, "    sll  $t%d, $t%d, %d         # %s = %s * %s (shift)\n",
                                    d, a, sh, i->result, i->arg1, i->arg2);
                            break;
                        }
                    }
                }

                int a = operandReg(i->arg1);
                int b = operandReg(i->arg2);
                int d = defReg(i->result);
                fprintf(out, "    %-4s $t%d, $t%d, $t%d       # %s = %s %s %s\n",
                        mnemonicFor(i->op), d, a, b,
                        i->result, i->arg1, mnemonicFor(i->op), i->arg2);
                break;
            }

            case TAC_NEG: {
                int a = operandReg(i->arg1);
                int d = defReg(i->result);
                fprintf(out, "    sub  $t%d, $zero, $t%d     # %s = -%s\n", d, a, i->result, i->arg1);
                break;
            }


            /* ---------- assignment --------------------------------------- */
            case TAC_ASSIGN: {
                int a = operandReg(i->arg1);
                int d = defReg(i->result);
                if (a != d) fprintf(out, "    move $t%d, $t%d            # %s = %s\n", d, a, i->result, i->arg1);
                break;
            }

            /* ---------- print: syscall 1 (int) then syscall 4 (newline) --- */
            case TAC_PRINT: {
                int a = operandReg(i->arg1);
                fprintf(out, "    move $a0, $t%d            # print %s\n", a, i->arg1);
                fprintf(out, "    li   $v0, 1\n    syscall\n");
                fprintf(out, "    la   $a0, __nl\n    li   $v0, 4\n    syscall\n");
                break;
            }

            /* ---------- control flow --------------------------------------
             * THE REGISTER CACHE IS FORGOTTEN AT EVERY LABEL AND BRANCH.
             * Two different paths can reach one label with different values
             * cached in $t0-$t9 (the fall-through path vs. the back edge), so
             * the only state both can agree on is memory.  Flushing writes
             * every dirty value home and empties the cache; skip it and loops
             * compute beautiful, wrong answers. */
            case TAC_LABEL:
                /* Flush BEFORE the label so the fall-through path writes back;
                 * code after the label then starts with an empty cache. */
                flushRegistersMode("label: paths may join here", FLUSH_NO_TEMPS);
                fprintf(out, "%s:\n", i->result);
                break;

            case TAC_GOTO:
                flushRegistersMode("before jump", FLUSH_NO_TEMPS); /* target expects memory current */
                fprintf(out, "    j    %s\n", i->arg1);
                break;

            case TAC_IF_FALSE:
            case TAC_IF_TRUE: {
                /* ORDER MATTERS: read the condition into a register FIRST,
                 * then flush.  The flush only emits stores (it does not
                 * change $t values), so the branch can still use the register;
                 * but the stores must come BEFORE the branch, because a taken
                 * branch would skip them. */
                if (fusedCompare) {
                    /* Fused with the comparison just before (see PEEPHOLE:
                     * COMPARE-AND-BRANCH FUSION).  IF_FALSE jumps when the
                     * comparison is FALSE, so it uses the inverse relation. */
                    TACInstr* cmp = fusedCompare;
                    fusedCompare = NULL;
                    int jumpIfTrue = (i->op == TAC_IF_TRUE);
                    const char* br;
                    switch (cmp->op) {
                        case TAC_LT: br = jumpIfTrue ? "blt" : "bge"; break;
                        case TAC_GT: br = jumpIfTrue ? "bgt" : "ble"; break;
                        case TAC_LE: br = jumpIfTrue ? "ble" : "bgt"; break;
                        case TAC_GE: br = jumpIfTrue ? "bge" : "blt"; break;
                        case TAC_EQ: br = jumpIfTrue ? "beq" : "bne"; break;
                        default:     br = jumpIfTrue ? "bne" : "beq"; break;
                    }
                    int a = operandReg(cmp->arg1);
                    char rhs[32];
                    if (isConstant(cmp->arg2)) snprintf(rhs, sizeof rhs, "%s", cmp->arg2);
                    else snprintf(rhs, sizeof rhs, "$t%d", operandReg(cmp->arg2));
                    flushBeforeBranch();
                    fprintf(out, "    %-4s $t%d, %s, %s   # %s %s (fused: %s %s %s)\n",
                            br, a, rhs, i->arg2,
                            i->op == TAC_IF_FALSE ? "if_false" : "if_true", i->arg1,
                            cmp->arg1, mnemonicFor(cmp->op), cmp->arg2);
                    break;
                }
                int a = operandReg(i->arg1);
                flushBeforeBranch();
                fprintf(out, "    %-4s $t%d, $zero, %s   # %s %s\n",
                        i->op == TAC_IF_FALSE ? "beq" : "bne", a, i->arg2,
                        i->op == TAC_IF_FALSE ? "if_false" : "if_true", i->arg1);
                break;
            }

            /* ---------- calls --------------------------------------------- */
            case TAC_ARG:
                if (argBufCount < (int)(sizeof argBuf / sizeof argBuf[0]))
                    argBuf[argBufCount++] = i->arg1;
                break;

            case TAC_CALL: {
                int n = atoi(i->arg2 ? i->arg2 : "0");
                if (n > MAX_ARGS) {
                    fprintf(stderr, "codegen: call to '%s' passes %d arguments; "
                                    "this compiler supports at most %d ($a0-$a3).\n",
                            i->arg1, n, MAX_ARGS);
                    exit(1);
                }
                /* A call consumes the LAST n buffered arguments, which keeps
                 * nested calls such as f(1, g(2)) in the right order. */
                int first = argBufCount - n;
                if (first < 0) first = 0;

                /* Materialise arguments into $a0-$a3 while $t regs are still
                 * valid, THEN flush, THEN jump. */
                for (int k = 0; k < n; k++) {
                    const char* a = argBuf[first + k];
                    Symbol* s = lookupSymbol(a);
                    if (s && s->isArray) {
                        /* Arrays are passed by reference: hand over the base address */
                        int r = arrayBaseReg(a);
                        fprintf(out, "    move $a%d, $t%d            # argument %s (array base)\n", k, r, a);
                        releaseReg(r);
                    } else {
                        int r = operandReg(a);
                        fprintf(out, "    move $a%d, $t%d            # argument %s\n", k, r, a);
                    }
                }
                argBufCount = first;

                flushRegisters("caller-saved registers before call");
                fprintf(out, "    jal  %s\n", funcLabel(i->arg1));

                int d = defReg(i->result);
                fprintf(out, "    move $t%d, $v0            # %s = return value\n", d, i->result);
                break;
            }

            case TAC_RETURN:
                if (i->arg1) {
                    int a = operandReg(i->arg1);
                    fprintf(out, "    move $v0, $t%d            # return %s\n", a, i->arg1);
                }
                flushRegistersMode("before return", FLUSH_GLOBALS);
                fprintf(out, "    j    %s__epilogue\n", funcLabel(currentFunc));
                break;

            /* ---------- arrays -------------------------------------------- */
            case TAC_ARRAY_LOAD: {                /* result = arr[index] */
                int addr = elementAddrReg(i->arg1, i->arg2);
                int d    = defReg(i->result);
                fprintf(out, "    lw   $t%d, 0($t%d)         # %s = %s[%s]\n",
                        d, addr, i->result, i->arg1, i->arg2);
                releaseReg(addr);
                break;
            }
            case TAC_ARRAY_STORE: {               /* arr[index] = value */
                int val  = operandReg(i->result);
                int addr = elementAddrReg(i->arg1, i->arg2);
                fprintf(out, "    sw   $t%d, 0($t%d)         # %s[%s] = %s\n",
                        val, addr, i->arg1, i->arg2, i->result);
                releaseReg(addr);
                break;
            }

            default:
                break;
            }
        }

        flushRegistersMode("end of function body", FLUSH_GLOBALS);
        emitEpilogue(currentFunc, frameSize);
    }

    fclose(out);
}
