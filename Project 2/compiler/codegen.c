/* =========================================================================
 * CST-405  ·  TOPIC 2  ·  Compiler for a Starter Language
 * FILE: codegen.c   —   Phase 6 — MIPS code generation
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                                                  ^^^^^^^  this file
 *
 *   RECEIVES : the OPTIMIZED TAC list from tac.c
 *   PRODUCES : a MIPS .s file that runs in SPIM / QtSPIM (after a peephole pass)
 *
 * WHAT IS NEW IN TOPIC 2
 *   • TAC -> MIPS: a register cache over memory homes, and syscalls for print
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


static int frameSize  = 0;        /* Size of the frame being generated       */
static const char* currentFunc = NULL;

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

/* Least-recently-used register — the one whose value we can most afford
 * to lose.  A real compiler would use liveness information here; LRU is a
 * good approximation and is easy to reason about in class. */
static int selectVictimReg(void) {
    int victim = 0, oldest = regAlloc.regs[0].lastUsed;
    for (int i = 1; i < NUM_TEMP_REGS; i++) {
        /* Values with no memory home are the cheapest to discard */
        if (!regAlloc.regs[i].hasHome && regAlloc.regs[i].inUse) return i;
        if (regAlloc.regs[i].lastUsed < oldest) {
            oldest = regAlloc.regs[i].lastUsed;
            victim = i;
        }
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
    if (r != -1) return r;                       /* Already cached          */

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

static void releaseReg(int r) {
    if (r < 0) return;
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
        if (r != -1) return r;
        r = getReg(n, 0, 0);
        fprintf(out, "    li   $t%d, %s\n", r, s);
        return r;
    }
    return ensureReg(s);
}

/* Write every dirty register back to memory, then forget all cached values.
 * Called before `jal` (the callee owns $t0-$t9) and before any label, since
 * control may reach a label from somewhere with a different cache state. */
static void flushRegisters(const char* why) {
    int any = 0;
    for (int i = 0; i < NUM_TEMP_REGS; i++)
        if (regAlloc.regs[i].inUse && regAlloc.regs[i].isDirty && regAlloc.regs[i].hasHome) any = 1;
    if (any) fprintf(out, "    # write back live values (%s)\n", why);
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        spillReg(i);
        releaseReg(i);
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

/* ==========================================================================
 * PART 5 — PEEPHOLE OPTIMIZATION (on the generated MIPS)
 * --------------------------------------------------------------------------
 * The TAC optimizer (tac.c) works on the program's MEANING.  Some waste only
 * becomes visible once real instructions exist — it is created by the
 * translation itself.  A peephole optimizer slides a small window (here: two
 * adjacent instructions) over the finished assembly and replaces patterns it
 * recognises with something cheaper.  It knows nothing about the source
 * language; it only knows MIPS.
 *
 *   PATTERN                                   REWRITE
 *   ---------------------------------------   ------------------------------
 *   1. move $X, $X                            (deleted — does nothing)
 *   2. j    L            followed by   L:     (deleted — jump to next line;
 *                                              this is the FLOW optimization
 *                                              that every function's return
 *                                              produces)
 *   3. sw   $r, A        then  lw $r, A       lw deleted — $r already holds it
 *   4. move $a, $b       then  move $b, $a    second move deleted
 *   5. li   $tA, K       then  move $R, $tA   li $R, K   (only when $tA is
 *                                              never mentioned again, so the
 *                                              scratch register was pointless)
 *
 * The window runs repeatedly until nothing changes, exactly like the TAC
 * optimizer: one rewrite can expose another.
 *
 * KNOWN LIMITATIONS
 *   • The window is two instructions wide, so a pattern split by a third
 *     instruction is missed.
 *   • Pattern 5 proves $tA dead by scanning the REST OF THE FILE, not just
 *     the basic block — safe but conservative (a reuse of $tA in a later
 *     function blocks the rewrite).
 *   • Lines longer than PH_LINE_LEN would be split; the generator never
 *     emits lines that long.
 * ========================================================================*/
#define PH_MAX_LINES 20000
#define PH_LINE_LEN  256

static char* phLines[PH_MAX_LINES];
static int   phCount;
static int   phStats[6];                 /* applications of patterns 1..5   */

/* The instruction part of a line: leading blanks skipped, comment removed,
 * trailing blanks removed.  Written into `dst`. */
static void phCore(const char* line, char* dst, size_t n) {
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (line[i] && line[i] != '#' && line[i] != '\n' && i < n - 1) { dst[i] = line[i]; i++; }
    while (i > 0 && (dst[i-1] == ' ' || dst[i-1] == '\t')) i--;
    dst[i] = '\0';
}

/* Split "op a, b, c" into op and up to three operands (whitespace trimmed).
 * Returns the number of operands found. */
static int phSplit(const char* core, char* op, char args[3][64]) {
    op[0] = args[0][0] = args[1][0] = args[2][0] = '\0';
    if (sscanf(core, "%15s", op) != 1) return 0;
    const char* p = core + strlen(op);
    int n = 0;
    while (*p && n < 3) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != ',' && k < 63) args[n][k++] = *p++;
        while (k > 0 && (args[n][k-1] == ' ' || args[n][k-1] == '\t')) k--;
        args[n][k] = '\0';
        n++;
    }
    return n;
}

/* Is `reg` (e.g. "$t4") mentioned in the instruction part of any line from
 * index `from` onward?  Used to prove a scratch register is dead. */
static int phRegUsedFrom(int from, const char* reg) {
    char core[PH_LINE_LEN];
    size_t len = strlen(reg);
    for (int i = from; i < phCount; i++) {
        if (!phLines[i]) continue;
        phCore(phLines[i], core, sizeof core);
        for (char* hit = strstr(core, reg); hit; hit = strstr(hit + 1, reg))
            if (hit[len] < '0' || hit[len] > '9') return 1;   /* $t1 is not $t10 */
    }
    return 0;
}

/* Index of the next line holding an instruction or label (skipping blank
 * and comment-only lines), or -1. */
static int phNext(int i) {
    char core[PH_LINE_LEN];
    for (int j = i + 1; j < phCount; j++) {
        if (!phLines[j]) continue;
        phCore(phLines[j], core, sizeof core);
        if (core[0]) return j;
    }
    return -1;
}

static void phDelete(int i, const char* why) {
    char core[PH_LINE_LEN];
    phCore(phLines[i], core, sizeof core);
    trace("    removed   %-28s  (%s)\n", core, why);
    free(phLines[i]);
    phLines[i] = NULL;
}

static void peepholeOptimizeFile(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return;
    char buf[PH_LINE_LEN];
    phCount = 0;
    memset(phStats, 0, sizeof phStats);
    while (phCount < PH_MAX_LINES && fgets(buf, sizeof buf, f))
        phLines[phCount++] = strdup(buf);
    fclose(f);

    trace("\n  Peephole optimization (2-instruction window over the MIPS):\n");

    int changed;
    do {
        changed = 0;
        for (int i = 0; i < phCount; i++) {
            if (!phLines[i]) continue;
            char c1[PH_LINE_LEN], c2[PH_LINE_LEN];
            char op1[16], op2[16], a1[3][64], a2[3][64];
            phCore(phLines[i], c1, sizeof c1);
            if (!c1[0]) continue;
            int n1 = phSplit(c1, op1, a1);

            /* 1. move $X, $X */
            if (strcmp(op1, "move") == 0 && n1 == 2 && strcmp(a1[0], a1[1]) == 0) {
                phDelete(i, "self-move");
                phStats[1]++; changed = 1; continue;
            }

            int j = phNext(i);
            if (j < 0) continue;
            phCore(phLines[j], c2, sizeof c2);
            int n2 = phSplit(c2, op2, a2);

            /* 2. j L ; L:   — jump to the next instruction */
            if (strcmp(op1, "j") == 0 && n1 == 1) {
                size_t L = strlen(a1[0]);
                if (strncmp(c2, a1[0], L) == 0 && c2[L] == ':' && c2[L+1] == '\0') {
                    phDelete(i, "jump to next instruction");
                    phStats[2]++; changed = 1; continue;
                }
            }

            /* Patterns 3-5 need two instructions back to back with no label
             * between them (a label means control can arrive in the middle). */
            if (c2[strlen(c2) - 1] == ':') continue;

            /* 3. sw $r, A ; lw $r, A */
            if (strcmp(op1, "sw") == 0 && strcmp(op2, "lw") == 0 && n1 == 2 && n2 == 2 &&
                strcmp(a1[0], a2[0]) == 0 && strcmp(a1[1], a2[1]) == 0) {
                phDelete(j, "value already in register");
                phStats[3]++; changed = 1; continue;
            }

            /* 4. move $a, $b ; move $b, $a */
            if (strcmp(op1, "move") == 0 && strcmp(op2, "move") == 0 && n1 == 2 && n2 == 2 &&
                strcmp(a1[0], a2[1]) == 0 && strcmp(a1[1], a2[0]) == 0) {
                phDelete(j, "move back and forth");
                phStats[4]++; changed = 1; continue;
            }

            /* 5. li $tA, K ; move $R, $tA   with $tA dead afterwards */
            if (strcmp(op1, "li") == 0 && strcmp(op2, "move") == 0 && n1 == 2 && n2 == 2 &&
                strcmp(a1[0], a2[1]) == 0 && !phRegUsedFrom(j + 1, a1[0])) {
                char repl[PH_LINE_LEN];
                trace("    combined  %-28s  +  %s\n", c1, c2);
                snprintf(repl, sizeof repl, "    li   %s, %s        # peephole: was li %s + move\n",
                         a2[0], a1[1], a1[0]);
                free(phLines[i]); phLines[i] = NULL;
                free(phLines[j]); phLines[j] = strdup(repl);
                phStats[5]++; changed = 1; continue;
            }
        }
    } while (changed);

    int total = phStats[1] + phStats[2] + phStats[3] + phStats[4] + phStats[5];
    if (total == 0) trace("    (no opportunities found)\n");
    trace("  Peephole rewrites: %d  [self-move %d, jump-to-next %d, redundant load %d,"
          " move pair %d, li+move %d]\n",
          total, phStats[1], phStats[2], phStats[3], phStats[4], phStats[5]);

    f = fopen(filename, "w");
    if (!f) return;
    for (int i = 0; i < phCount; i++)
        if (phLines[i]) { fputs(phLines[i], f); free(phLines[i]); }
    fclose(f);
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
        initRegAlloc();

        emitPrologue(currentFunc, frameSize);

        for (TACInstr* i = c->next; i && i->op != TAC_FUNC_END; i = i->next) {
            /* --------------------------------------------------------
             * TODO (Topic 2) — TAC -> MIPS -- Finished
             * One case per TAC opcode.  Everything you need is already written above:
             *
             *     operandReg(name)   register holding that value (loads it, or does
             *                        `li` if it is a literal)
             *     defReg(name)       register to WRITE a new value of `name` into
             *     flushRegisters()   write every dirty register back to memory
             *
             *   TAC_DECL     no instruction — the slot was reserved by layoutFrame().
             *                Emit a comment saying where it lives; you will be glad of
             *                it the first time you read your own assembly.
             *
             *   TAC_ASSIGN   result = arg1
             *                    int a = operandReg(i->arg1);
             *                    int d = defReg(i->result);
             *                    move $td, $ta
             *
             *   TAC_ADD      result = arg1 + arg2   ->   add $td, $ta, $tb
             *
             *   TAC_PRINT    print arg1, using the SPIM syscalls:
             *                    move $a0, $t<arg>
             *                    li   $v0, 1        # 1 = print integer
             *                    syscall
             *                    la   $a0, __nl     # then a newline
             *                    li   $v0, 4        # 4 = print string
             *                    syscall
             *
             *   TAC_RETURN   put the value in $v0, then jump to the epilogue label.
             *                Do NOT just fall through.
             *
             * WHY defReg AND operandReg ARE DIFFERENT
             *   operandReg must LOAD the value from memory if it is not already in a
             *   register.  defReg must not: the register is about to be overwritten,
             *   so loading first is a wasted instruction.  Use the wrong one and your
             *   code still works — just with an extra `lw` everywhere.  Reading your
             *   own output and spotting that is a genuinely good exercise.
             * -------------------------------------------------------- */
         {
                int used = 0;
                for (int r = 0; r < NUM_TEMP_REGS; r++)
                    if (regAlloc.regs[r].inUse) used++;
                if (used > NUM_TEMP_REGS - 3) flushRegisters("make room");
            }
 
            switch (i->op) {
                case TAC_DECL: {
                    Symbol* s = lookupSymbol(i->result);
                    if (s) fprintf(out, "    # int %s lives at %d($sp)\n", s->name, s->offset);
                    break;
                }
 
                case TAC_ASSIGN: {
                    int a = operandReg(i->arg1);
                    int d = defReg(i->result);
                    fprintf(out, "    move $t%d, $t%d        # %s = %s\n", d, a, i->result, i->arg1);
                    break;
                }
 
                case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV: {
                    int a = operandReg(i->arg1);
                    int b = operandReg(i->arg2);
                    int d = defReg(i->result);
                    fprintf(out, "    %s  $t%d, $t%d, $t%d        # %s = %s %s %s\n",
                            mnemonicFor(i->op), d, a, b,
                            i->result, i->arg1,
                            i->op == TAC_ADD ? "+" : i->op == TAC_SUB ? "-" :
                            i->op == TAC_MUL ? "*" : "/", i->arg2);
                    break;
                }
 
                case TAC_PRINT: {
                    int a = operandReg(i->arg1);
                    fprintf(out, "    move $a0, $t%d        # print(%s)\n", a, i->arg1);
                    fprintf(out, "    li   $v0, 1             # syscall 1 = print integer\n");
                    fprintf(out, "    syscall\n");
                    fprintf(out, "    la   $a0, __nl\n");
                    fprintf(out, "    li   $v0, 4             # syscall 4 = print string\n");
                    fprintf(out, "    syscall\n");
                    break;
                }
 
                case TAC_RETURN: {
                    if (i->arg1) {
                        int a = operandReg(i->arg1);
                        fprintf(out, "    move $v0, $t%d        # return value\n", a);
                    }
                    flushRegisters("before return");   /* globals must reach memory */
                    fprintf(out, "    j    %s__epilogue\n", funcLabel(currentFunc));
                    break;
                }
 
                default:
                    fprintf(out, "    # (TAC opcode %d is not generated in Topic 2)\n", (int)i->op);
                    break;
            }
        }
 
        flushRegisters("end of function body");
        emitEpilogue(currentFunc, frameSize);
    }
 
    fclose(out);

    /* Last step of the back end: clean up the finished assembly. */
    peepholeOptimizeFile(filename);
}
 
