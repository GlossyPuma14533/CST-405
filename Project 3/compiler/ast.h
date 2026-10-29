/* =========================================================================
 * CST-405  ·  TOPIC 3  ·  Compiling Complex Variables and Functions
 * FILE: ast.h   —   Phase 2 — Syntax analysis (the tree it builds)
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                        ^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • New nodes: FUNC_DEF, PARAM, PARAM_LIST, FUNC_CALL, ARG_LIST, RETURN
 *   • New nodes: ARRAY_DECL, ARRAY_INDEX, BLOCK
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 *
 * STATUS
 *   Every TODO (Topic 3) in this file has been implemented: nine new node
 *   kinds, their union members, and their constructor prototypes.
 * ========================================================================= */

#ifndef AST_H
#define AST_H

/* ABSTRACT SYNTAX TREE (AST)
 * The AST is an intermediate representation of the program structure
 * It represents the hierarchical syntax of the source code
 * Each node represents a construct in the language
 */

/* NODE TYPES - Different kinds of AST nodes in our language */
typedef enum {
    NODE_NUM,         /* Numeric literal (e.g., 42) */
    NODE_VAR,         /* Variable reference (e.g., x) */
    NODE_BINOP,       /* Binary operation (e.g., x + y) */
    NODE_DECL,        /* Variable declaration (e.g., int x) */
    NODE_ASSIGN,      /* Assignment statement (e.g., x = 10) */
    NODE_PRINT,       /* Print statement (e.g., print(x)) */
    NODE_STMT_LIST,   /* List of statements (program structure) */
    /* ---- Topic 3: functions ---------------------------------------------- */
    NODE_FUNC_DEF,    /* Function definition: int f(params) { body }        */
    NODE_PARAM,       /* One SCALAR parameter: int a                        */
    NODE_PARAM_LIST,  /* Linked list of parameters, in source order         */
    NODE_FUNC_CALL,   /* Call used as a value or as a statement: f(1, x)    */
    NODE_ARG_LIST,    /* Linked list of argument expressions, source order  */
    NODE_RETURN,      /* return expr;  or  return;                          */
    /* ---- Topic 3: blocks and arrays -------------------------------------- */
    NODE_BLOCK,       /* { stmt_list } — opens a new scope                  */
    NODE_ARRAY_DECL,  /* int a[10];  or the array PARAMETER  int a[]        */
    NODE_ARRAY_INDEX  /* a[expr] — an array element used as a value/target  */
} NodeType;

/* AST NODE STRUCTURE
 * Uses a union to efficiently store different node data
 * Only the relevant fields for each node type are used
 */
typedef struct ASTNode {
    NodeType type;  /* Identifies what kind of node this is */
    int lineno;     /* Line number in source code for error reporting */

    /* Union allows same memory to store different data types */
    union {
        /* Literal number value (NODE_NUM) */
        int num;
        
        /* Variable reference name (NODE_VAR) */
        char* name;

        /* Declaration structure (NODE_DECL) */
        struct {
            char* name;
            char* varType;
        } decl;


        /* Binary operation structure (NODE_BINOP) */
        struct {
            char op;                    /* Operator character ('+') */
            struct ASTNode* left;       /* Left operand */
            struct ASTNode* right;      /* Right operand */
        } binop;
        
        /* Assignment structure (NODE_ASSIGN) */
        struct {
            char* var;                  /* Variable being assigned to (NULL for array) */
            struct ASTNode* value;      /* Expression being assigned */
            struct ASTNode* arrayLHS;   /* Topic 3: NODE_ARRAY_INDEX target of
                                         * arr[i] = expr;  NULL for x = expr; */
        } assign;
        
        /* Print expression (NODE_PRINT) */
        struct ASTNode* expr;
        
        /* Statement list structure (NODE_STMT_LIST) */
        struct {
            struct ASTNode* stmt;       /* Current statement */
            struct ASTNode* next;       /* Rest of the list */
        } stmtlist;

        /* ---- Topic 3: function-related nodes ----------------------------
         * Each records exactly what later phases ask for and nothing more. */

        /* Function definition (NODE_FUNC_DEF) */
        struct {
            char* name;                 /* Function name                      */
            struct ASTNode* params;     /* NODE_PARAM_LIST, or NULL for f()   */
            struct ASTNode* body;       /* NODE_BLOCK                         */
        } func_def;

        /* Scalar parameter (NODE_PARAM).  Array parameters reuse
         * NODE_ARRAY_DECL with isParam = 1. */
        struct {
            char* name;
        } param;

        /* Parameter list (NODE_PARAM_LIST) — right-linked, source order */
        struct {
            struct ASTNode* param;      /* NODE_PARAM or NODE_ARRAY_DECL      */
            struct ASTNode* next;       /* Rest of the list, or NULL          */
        } param_list;

        /* Function call (NODE_FUNC_CALL) */
        struct {
            char* name;                 /* Callee name                        */
            struct ASTNode* args;       /* NODE_ARG_LIST, or NULL for f()     */
        } func_call;

        /* Argument list (NODE_ARG_LIST) — right-linked, source order */
        struct {
            struct ASTNode* expr;       /* This argument's expression         */
            struct ASTNode* next;       /* Rest of the list, or NULL          */
        } arg_list;

        /* Return statement (NODE_RETURN) */
        struct {
            struct ASTNode* expr;       /* NULL for a bare `return;`          */
        } ret;

        /* ---- Topic 3: blocks and arrays -------------------------------- */

        /* Block (NODE_BLOCK) */
        struct {
            struct ASTNode* stmt_list;  /* NULL for an empty block {}         */
        } block;

        /* Array declaration or array parameter (NODE_ARRAY_DECL).
         * isParam separates `int a[10];` (reserve 40 bytes of storage) from
         * `int a[]` as a parameter (reserve 4 bytes for an address). */
        struct {
            char* name;
            int   size;                 /* Element count; 0 for a parameter   */
            int   isParam;              /* 1 = `int a[]` parameter            */
        } array_decl;

        /* Array element (NODE_ARRAY_INDEX).  The array is a NAME, not an
         * expression node: this language has no pointers, so the only thing
         * that can be indexed is a declared array. */
        struct {
            char* name;
            struct ASTNode* index;      /* Index expression                   */
        } array_index;
    } data;
} ASTNode;

/* AST CONSTRUCTION FUNCTIONS
 * These functions are called by the parser to build the tree
 */
/* Basic nodes */
ASTNode* createNum(int value);                                   /* Create number node */
ASTNode* createVar(char* name);                                  /* Create variable node */
ASTNode* createBinOp(char op, ASTNode* left, ASTNode* right);   /* Create binary op node */
ASTNode* createDecl(char* type, char* name);                     /* Create declaration node */
ASTNode* createAssign(char* var, ASTNode* value);               /* Create assignment node */
ASTNode* createPrint(ASTNode* expr);                            /* Create print node */
ASTNode* createStmtList(ASTNode* stmt1, ASTNode* stmt2);        /* Create statement list */

/* Function-related nodes */
ASTNode* createFuncDef(char* name, ASTNode* params, ASTNode* body);
ASTNode* createParam(char* name);                               /* int a        */
ASTNode* createParamList(ASTNode* param, ASTNode* next);
ASTNode* appendParam(ASTNode* list, ASTNode* param);            /* keep source order */
ASTNode* createFuncCall(char* name, ASTNode* args);
ASTNode* createArgList(ASTNode* expr, ASTNode* next);
ASTNode* appendArg(ASTNode* list, ASTNode* expr);               /* keep source order */
ASTNode* createReturn(ASTNode* expr);                           /* expr may be NULL */

/* Control flow nodes */
ASTNode* createBlock(ASTNode* stmtList);                        /* stmtList may be NULL */

/* Array nodes.  createArrayParam is separate from createArrayDecl: a
 * parameter has no size and is a reference, not storage. */
ASTNode* createArrayDecl(char* name, int size);                 /* int a[10];   */
ASTNode* createArrayParam(char* name);                          /* int a[]      */
ASTNode* createArrayIndex(char* name, ASTNode* index);          /* a[i]         */
ASTNode* createArrayAssign(ASTNode* target, ASTNode* value);    /* a[i] = expr; */

/* Text form of a packed operator code (see createBinOp) */
const char* opText(char op);

/* AST DISPLAY FUNCTION */
void printAST(ASTNode* node, int level);                        /* Pretty-print the AST */

#endif