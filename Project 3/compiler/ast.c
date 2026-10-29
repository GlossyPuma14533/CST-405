/* =========================================================================
 * CST-405  ·  TOPIC 3  ·  Compiling Complex Variables and Functions
 * FILE: ast.c   —   Phase 2 — Syntax analysis (the tree it builds)
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                        ^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • Constructors and printing for every new node type
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 *
 * STATUS
 *   Every TODO (Topic 3) in this file has been implemented (constructors
 *   and printAST cases for all nine new node types).
 * ========================================================================= */

/* AST IMPLEMENTATION
 * Functions to create and manipulate Abstract Syntax Tree nodes
 * The AST is built during parsing and used for all subsequent phases
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

/* External line number from scanner */
extern int yylineno;

/* Create a number literal node */
ASTNode* createNum(int value) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_NUM;
    node->lineno = yylineno;
    node->data.num = value;  /* Store the integer value */
    return node;
}

/* Create a variable reference node */
ASTNode* createVar(char* name) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_VAR;
    node->lineno = yylineno;
    node->data.name = strdup(name);  /* Copy the variable name */
    return node;
}

/* Create a binary operation node (for addition) */
ASTNode* createBinOp(char op, ASTNode* left, ASTNode* right) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_BINOP;
    node->lineno = yylineno;
    node->data.binop.op = op;        /* Store operator (+) */
    node->data.binop.left = left;    /* Left subtree */
    node->data.binop.right = right;  /* Right subtree */
    return node;
}

/* Create a variable declaration node */
ASTNode* createDecl(char* type, char* name) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_DECL;
    node->lineno = yylineno;
    node->data.decl.name = strdup(name);
    node->data.decl.varType = strdup(type);
    return node;
}

/* Create an assignment statement node */
ASTNode* createAssign(char* var, ASTNode* value) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_ASSIGN;
    node->lineno = yylineno;
    node->data.assign.var = var ? strdup(var) : NULL;  /* Variable name (NULL for array assignments) */
    node->data.assign.value = value;      /* Expression tree */
    /* No array subscript on the left unless the parser says so */
    node->data.assign.arrayLHS = NULL;
    return node;
}

/* Create a print statement node */
ASTNode* createPrint(ASTNode* expr) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_PRINT;
    node->lineno = yylineno;
    node->data.expr = expr;  /* Expression to print */
    return node;
}

/* Create a statement list node (links statements together) */
ASTNode* createStmtList(ASTNode* stmt1, ASTNode* stmt2) {
    ASTNode* node = malloc(sizeof(ASTNode));
    node->type = NODE_STMT_LIST;
    node->lineno = stmt1 ? stmt1->lineno : (stmt2 ? stmt2->lineno : yylineno);
    node->data.stmtlist.stmt = stmt1;  /* First statement */
    node->data.stmtlist.next = stmt2;  /* Rest of list */
    return node;
}

/* ==========================================================================
 * TOPIC 3 — FUNCTION-RELATED NODES
 * --------------------------------------------------------------------------
 * Every constructor below is the same three steps — allocate, set the type
 * and line number, store the children.  The AST is deliberately boring so
 * the phases above it (semantic, TAC, codegen) can be interesting.
 * ========================================================================*/

/* Shared allocation helper: one place that sets type and line number. */
static ASTNode* newNode(NodeType type) {
    ASTNode* node = calloc(1, sizeof(ASTNode));   /* calloc: all children NULL */
    node->type = type;
    node->lineno = yylineno;
    return node;
}

/* int name(params) body  —  params is a NODE_PARAM_LIST or NULL */
ASTNode* createFuncDef(char* name, ASTNode* params, ASTNode* body) {
    ASTNode* node = newNode(NODE_FUNC_DEF);
    node->data.func_def.name   = strdup(name);
    node->data.func_def.params = params;
    node->data.func_def.body   = body;
    return node;
}

/* A scalar parameter:  int a  (passed BY VALUE in $a0-$a3) */
ASTNode* createParam(char* name) {
    ASTNode* node = newNode(NODE_PARAM);
    node->data.param.name = strdup(name);
    return node;
}

/* One cell of the parameter list */
ASTNode* createParamList(ASTNode* param, ASTNode* next) {
    ASTNode* node = newNode(NODE_PARAM_LIST);
    node->lineno = param ? param->lineno : yylineno;
    node->data.param_list.param = param;
    node->data.param_list.next  = next;
    return node;
}

/* Append to the END of a parameter list.  The grammar rule is
 * left-recursive (param_list ',' param), so the list arrives one item at a
 * time from the left; appending keeps it in SOURCE order, which matters
 * because parameter position decides which $a register it arrives in. */
ASTNode* appendParam(ASTNode* list, ASTNode* param) {
    ASTNode* cell = createParamList(param, NULL);
    if (!list) return cell;
    ASTNode* p = list;
    while (p->data.param_list.next) p = p->data.param_list.next;
    p->data.param_list.next = cell;
    return list;
}

/* name(args) — args is a NODE_ARG_LIST or NULL */
ASTNode* createFuncCall(char* name, ASTNode* args) {
    ASTNode* node = newNode(NODE_FUNC_CALL);
    node->data.func_call.name = strdup(name);
    node->data.func_call.args = args;
    return node;
}

/* One cell of the argument list */
ASTNode* createArgList(ASTNode* expr, ASTNode* next) {
    ASTNode* node = newNode(NODE_ARG_LIST);
    node->lineno = expr ? expr->lineno : yylineno;
    node->data.arg_list.expr = expr;
    node->data.arg_list.next = next;
    return node;
}

/* Append an argument, keeping source order (argument order is part of the
 * calling convention: argument 1 goes in $a0, argument 2 in $a1, ...). */
ASTNode* appendArg(ASTNode* list, ASTNode* expr) {
    ASTNode* cell = createArgList(expr, NULL);
    if (!list) return cell;
    ASTNode* p = list;
    while (p->data.arg_list.next) p = p->data.arg_list.next;
    p->data.arg_list.next = cell;
    return list;
}

/* return expr;  — expr is NULL for a bare  return; */
ASTNode* createReturn(ASTNode* expr) {
    ASTNode* node = newNode(NODE_RETURN);
    node->data.ret.expr = expr;
    return node;
}

/* ==========================================================================
 * TOPIC 3 — BLOCKS AND ARRAYS
 * ========================================================================*/

/* { stmt_list }  — stmtList is NULL for an empty block */
ASTNode* createBlock(ASTNode* stmtList) {
    ASTNode* node = newNode(NODE_BLOCK);
    node->data.block.stmt_list = stmtList;
    return node;
}

/* int name[size];  — real STORAGE: size*4 bytes.  isParam = 0. */
ASTNode* createArrayDecl(char* name, int size) {
    ASTNode* node = newNode(NODE_ARRAY_DECL);
    node->data.array_decl.name    = strdup(name);
    node->data.array_decl.size    = size;
    node->data.array_decl.isParam = 0;
    return node;
}

/* int name[]  as a parameter — a REFERENCE: one word holding the caller's
 * base address.  Size 0 (unknown to the callee) and isParam = 1.  Code
 * generation depends on telling this apart from createArrayDecl. */
ASTNode* createArrayParam(char* name) {
    ASTNode* node = newNode(NODE_ARRAY_DECL);
    node->data.array_decl.name    = strdup(name);
    node->data.array_decl.size    = 0;
    node->data.array_decl.isParam = 1;
    return node;
}

/* name[index] — the array NAME plus the index EXPRESSION.  The name is not
 * itself an expression node; that simplification is what keeps pointers out
 * of the language (you cannot index the result of a call, for example). */
ASTNode* createArrayIndex(char* name, ASTNode* index) {
    ASTNode* node = newNode(NODE_ARRAY_INDEX);
    node->data.array_index.name  = strdup(name);
    node->data.array_index.index = index;
    return node;
}

/* name[index] = value;  — an assignment whose target is an array element.
 * `var` stays NULL and the target tree lives in arrayLHS. */
ASTNode* createArrayAssign(ASTNode* target, ASTNode* value) {
    ASTNode* node = createAssign(NULL, value);
    node->data.assign.arrayLHS = target;
    return node;
}

/* Render an operator code as the text the programmer actually wrote.
 * The parser packs multi-character operators into single chars so that a
 * BINOP node stays small; this is where that encoding is undone. */
const char* opText(char op) {
    switch (op) {
        case '+': return "+";   case '-': return "-";
        case '*': return "*";   case '/': return "/";
        case '<': return "<";   case '>': return ">";
        case 'l': return "<=";  case 'g': return ">=";
        case 'e': return "==";  case 'n': return "!=";
        case 'u': return "unary -";
        case '&': return "&&";  case '|': return "||";  case '!': return "!";
        default:  return "?";
    }
}

/* Display the AST structure (for debugging and education) */
void printAST(ASTNode* node, int level) {
    if (!node) return;

    /* Indent based on tree depth */
    for (int i = 0; i < level; i++) printf("  ");

    /* Print node based on its type */
    switch(node->type) {
        case NODE_NUM:
            printf("NUM: %d\n", node->data.num);
            break;
        case NODE_VAR:
            printf("VAR: %s\n", node->data.name);
            break;
        case NODE_BINOP:
            printf("BINOP: %s\n", opText(node->data.binop.op));
            printAST(node->data.binop.left, level + 1);
            printAST(node->data.binop.right, level + 1);
            break;
        case NODE_DECL:
            printf("DECL: %s %s\n", node->data.decl.varType, node->data.decl.name);
            break;
        case NODE_ASSIGN:
            /* Two shapes: to a variable, or to an array element. */
            if (node->data.assign.arrayLHS) {
                printf("ASSIGN: array element\n");
                for (int i = 0; i < level + 1; i++) printf("  ");
                printf("TARGET:\n");
                printAST(node->data.assign.arrayLHS, level + 2);
            } else {
                printf("ASSIGN: %s\n", node->data.assign.var);
            }
            for (int i = 0; i < level + 1; i++) printf("  ");
            printf("VALUE:\n");
            printAST(node->data.assign.value, level + 2);
            break;
        case NODE_PRINT:
            printf("PRINT\n");
            printAST(node->data.expr, level + 1);
            break;
        case NODE_STMT_LIST:
            /* Print statements in sequence at same level */
            printAST(node->data.stmtlist.stmt, level);
            printAST(node->data.stmtlist.next, level);
            break;
        /* ---- Topic 3: function-related nodes (children one level deeper) */
        case NODE_FUNC_DEF:
            printf("FUNC_DEF: int %s(...)\n", node->data.func_def.name);
            if (node->data.func_def.params) {
                for (int i = 0; i < level + 1; i++) printf("  ");
                printf("PARAMS:\n");
                printAST(node->data.func_def.params, level + 2);
            }
            printAST(node->data.func_def.body, level + 1);
            break;
        case NODE_PARAM:
            printf("PARAM: int %s\n", node->data.param.name);
            break;
        case NODE_PARAM_LIST:
            /* A list prints as its items, all at the same depth */
            printAST(node->data.param_list.param, level);
            printAST(node->data.param_list.next, level);
            break;
        case NODE_FUNC_CALL:
            printf("CALL: %s\n", node->data.func_call.name);
            printAST(node->data.func_call.args, level + 1);
            break;
        case NODE_ARG_LIST:
            printf("ARG\n");
            printAST(node->data.arg_list.expr, level + 1);
            /* Remaining arguments are siblings, not children */
            if (node->data.arg_list.next) printAST(node->data.arg_list.next, level);
            break;
        case NODE_RETURN:
            printf("RETURN%s\n", node->data.ret.expr ? "" : " (no value)");
            printAST(node->data.ret.expr, level + 1);
            break;

        /* ---- Topic 3: blocks and arrays ----------------------------------- */
        case NODE_BLOCK:
            printf("BLOCK {\n");
            printAST(node->data.block.stmt_list, level + 1);
            for (int i = 0; i < level; i++) printf("  ");
            printf("}\n");
            break;
        case NODE_ARRAY_DECL:
            if (node->data.array_decl.isParam)
                printf("PARAM: int %s[]  (by reference)\n", node->data.array_decl.name);
            else
                printf("ARRAY_DECL: int %s[%d]\n", node->data.array_decl.name,
                       node->data.array_decl.size);
            break;
        case NODE_ARRAY_INDEX:
            printf("ARRAY_INDEX: %s[]\n", node->data.array_index.name);
            printAST(node->data.array_index.index, level + 1);
            break;
    }
}