/* =========================================================================
 * CST-405  ·  TOPIC 3  ·  Compiling Complex Variables and Functions
 * FILE: parser.y   —   Phase 2 — Syntax analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *              ^^^^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • A program is now a list of declarations AND function definitions
 *   • Array declaration `int a[10];`, indexing `a[i]`, and array parameters `int a[]`
 *   • Full arithmetic with precedence: + - * / , parentheses, unary minus
 *   • Function definitions, parameter lists, calls, and `return`
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 *
 * STATUS
 *   Every TODO (Topic 3) in this file has been implemented.  Each finished
 *   section is marked  DONE (Topic 3)  with a note on the design decision.
 * ========================================================================= */

%{
/* PHASE 2 — SYNTAX ANALYSIS
 *
 * The parser answers one question: do these tokens form a legal program?
 * Bison builds an LALR(1) bottom-up parser from the grammar below.  It reads
 * tokens left to right, shifts them onto a stack, and REDUCES whenever the
 * top of the stack matches the right-hand side of a rule.
 *
 * Answering "yes" is not enough, though — the rest of the compiler needs the
 * program's STRUCTURE.  So each rule carries a semantic action in { } that
 * builds one node of the abstract syntax tree.  $1, $2, ... are the values of
 * the symbols on the right-hand side; $$ is the value this rule hands back.
 *
 *      expr '+' expr   { $$ = createBinOp('+', $1, $3); }
 *       │        │                              │   └── right operand
 *       │        └── $3                         └────── left operand
 *       └── $1
 *
 * By the time yyparse() returns, the tree has been built bottom-up beneath us
 * and `root` points at the whole program.
 *
 * MEMORY NOTE: the scanner strdup()s every identifier.  Every AST constructor
 * copies the name again, so the rule that received ID from the scanner frees
 * it after building its node.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"

/* External declarations for lexer interface */
extern int yylex();
extern int yyparse();
extern FILE* yyin;
extern int yylineno;  /* Line number from scanner */

void yyerror(const char* s);
ASTNode* root = NULL;

/* DEFERRED SYNTAX ERRORS (carried forward from our Project 2 work)
 * Bison calls yyerror() the instant it hits a bad token — BEFORE it knows
 * whether one of the error productions below will recognise the mistake.
 * So yyerror() only REMEMBERS the generic message.  If an error production
 * fires, it prints a precise message ("missing ';' after ...") and discards
 * the generic one; otherwise main() calls flushSyntaxError() to print it.
 * Result: exactly one, as-specific-as-possible message per error. */
static char pendingError[512];
static int  pendingLine = 0;
static int  hasPendingError = 0;
static void discardPendingError(void) { hasPendingError = 0; }
void flushSyntaxError(void);
%}

/* Messages that say what was found and what was expected:
 *   "syntax error, unexpected ID, expecting ';' or '+'"  */
%define parse.error verbose

/* Track source locations (@n).  scanner.l fills yylloc via YY_USER_ACTION.
 * A missing ';' is only noticed at the NEXT token — often on the next line —
 * so @n lets us name the line that is actually wrong. */
%locations

/* SEMANTIC VALUES UNION */
%union {
    int num;
    char* str;
    struct ASTNode* node;
}

/* TOKEN DECLARATIONS */
%token <num> NUM
%token <str> ID
%token INT PRINT
/* DONE (Topic 3): functions need one more keyword token. */
%token RETURN

/* NON-TERMINAL TYPES */
%type <node> program stmt_list stmt decl assign expr print_stmt
/* DONE (Topic 3): types of the new non-terminals, so bison knows that their
 * $$ is an ASTNode*.  top_stmt is our addition (see TOP-LEVEL STATEMENTS). */
%type <node> decl_or_func_list decl_or_func func_def params param_list param
%type <node> return_stmt block func_call arg_list args top_stmt

/* OPERATOR PRECEDENCE AND ASSOCIATIVITY
 * Listed from lowest to highest precedence.
 *
 * DANGLING-ELSE RESOLUTION:
 *   The grammar has an ambiguity: in  if (c) if (c) s  else s
 *   the "else" can bind to either "if".  The standard resolution
 *   is that else binds to the nearest (innermost) if.
 *
 *   We express this by giving the if-without-else rule a lower
 *   precedence (%prec LOWER_THAN_ELSE) than the ELSE token, so
 *   when bison sees ELSE it shifts (attaches it to the inner if)
 *   rather than reducing (closing the outer if first).
 *   (if/else itself arrives in a later topic.)
 */
%left '+' '-'
%left '*' '/'
/* DONE (Topic 3): UMINUS is a precedence level with no token of its own.
 * The unary-minus rule borrows it with %prec, so  -2 * 3  parses as
 * (-2) * 3.  %right because  - -7  must group as  -(-7). */
%right UMINUS

%%

/* ==========================================================================
 * PROGRAM STRUCTURE                                        DONE (Topic 3)
 * --------------------------------------------------------------------------
 * A program is a list of top-level DECLARATIONS and FUNCTION DEFINITIONS:
 *   program           : decl_or_func_list
 *   decl_or_func_list : decl_or_func | decl_or_func_list decl_or_func
 *   decl_or_func      : func_def | decl
 * Executable statements live inside function bodies — which is why `main`
 * suddenly matters.
 *
 * TOP-LEVEL STATEMENTS (our compatibility extension)
 *   The milestone requires the Topic 2 tests to keep passing, and those are
 *   written as a flat list of statements with no main().  So decl_or_func
 *   also accepts the executable statement forms (top_stmt).  semantic.c and
 *   tac.c gather them into an IMPLICIT main(), and semantic.c rejects a
 *   program that mixes top-level statements with an explicit main().  The
 *   grammar stays LALR(1) with no conflicts: after `INT ID` the next token
 *   ( '(' , ';' or '[' ) decides between a function and a declaration.
 * ========================================================================*/
program:
    decl_or_func_list               { root = $1; }
    ;

decl_or_func_list:
    decl_or_func                    { $$ = $1; }
    | decl_or_func_list decl_or_func { $$ = createStmtList($1, $2); }
    ;

decl_or_func:
    func_def                        { $$ = $1; }
    | decl                          { $$ = $1; }
    | top_stmt                      { $$ = $1; }
    ;

/* Statements allowed at file scope (Topic 2 programs).  `return` parses here
 * on purpose: "return outside any function" is a SEMANTIC error, reported
 * with a line number, not a cryptic syntax error. */
top_stmt:
    assign                          { $$ = $1; }
    | print_stmt                    { $$ = $1; }
    | return_stmt                   { $$ = $1; }
    | func_call ';'                 { $$ = $1; }
    ;

/* ==========================================================================
 * FUNCTION DEFINITIONS, PARAMETERS AND BLOCKS              DONE (Topic 3)
 *   func_def   : INT ID '(' params ')' block  |  INT ID '(' ')' block
 *   param_list : param | param_list ',' param
 *   param      : INT ID           (a scalar parameter)
 *              | INT ID '[' ']'   (an array parameter — note: no size)
 *   block      : '{' stmt_list '}' | '{' '}'
 * An array parameter carries no size because arrays are passed by
 * REFERENCE: the callee receives an address, not a copy of the data.
 * ========================================================================*/
func_def:
    INT ID '(' params ')' block {
        $$ = createFuncDef($2, $4, $6);
        $$->lineno = @2.first_line;   /* report errors at the function name */
        free($2);
    }
    | INT ID '(' ')' block {
        $$ = createFuncDef($2, NULL, $5);
        $$->lineno = @2.first_line;
        free($2);
    }
    ;

params:
    param_list                      { $$ = $1; }
    ;

/* Left-recursive (bison's preference: constant stack depth), but appendParam
 * keeps the list in SOURCE order — parameter 1 must map to $a0. */
param_list:
    param                           { $$ = appendParam(NULL, $1); }
    | param_list ',' param          { $$ = appendParam($1, $3); }
    ;

param:
    INT ID                          { $$ = createParam($2); free($2); }
    | INT ID '[' ']'                { $$ = createArrayParam($2); free($2); }
    ;

block:
    '{' stmt_list '}'               { $$ = createBlock($2); }
    | '{' '}'                       { $$ = createBlock(NULL); }
    ;

/* STATEMENT LIST */
stmt_list:
    stmt {
        $$ = $1;
    }
    | stmt_list stmt {
        $$ = createStmtList($1, $2);
    }
    ;

/* STATEMENT TYPES
 * DONE (Topic 3): a `return`, a `{ ... }` block, and a bare function call
 * used for its effect are all statements now. */
stmt:
    decl
    | assign
    | print_stmt
    | return_stmt                   { $$ = $1; }
    | block                         { $$ = $1; }
    | func_call ';'                 { $$ = $1; }
    ;


/* DECLARATION - int x; or int arr[10]; */
decl:
    INT ID ';' {
        $$ = createDecl("int", $2);
        free($2);
    }
    /* DONE (Topic 3): array declaration.  The size is a NUM, not an
     * expression — the compiler must know how many bytes to reserve, and it
     * must know that at compile time. */
    | INT ID '[' NUM ']' ';' {
        $$ = createArrayDecl($2, $4);
        free($2);
    }
    /* Error production: `int x` with no ';' */
    | INT ID error {
        discardPendingError();
        fprintf(stderr, "Syntax Error at line %d: missing ';' after declaration of '%s'\n",
                @2.first_line, $2);
        free($2);
        $$ = NULL;
        YYABORT;
    }
    ;

/* ASSIGNMENT - x = expr; or arr[i] = expr; */
assign:
    ID '=' expr ';' {
        $$ = createAssign($1, $3);
        free($1);
    }
    /* DONE (Topic 3): assignment to an array ELEMENT.  The left-hand side is
     * now a tree, not just a name, so the node stores it in arrayLHS and
     * leaves `var` NULL. */
    | ID '[' expr ']' '=' expr ';' {
        $$ = createArrayAssign(createArrayIndex($1, $3), $6);
        free($1);
    }
    /* Error productions: missing ';' after either form */
    | ID '=' expr error {
        discardPendingError();
        fprintf(stderr, "Syntax Error at line %d: missing ';' after assignment to '%s'\n",
                @3.last_line, $1);
        free($1);
        $$ = NULL;
        YYABORT;
    }
    | ID '[' expr ']' '=' expr error {
        discardPendingError();
        fprintf(stderr, "Syntax Error at line %d: missing ';' after assignment to '%s[...]'\n",
                @6.last_line, $1);
        free($1);
        $$ = NULL;
        YYABORT;
    }
    ;

/* DONE (Topic 3): RETURN, in both forms. */
return_stmt:
    RETURN expr ';'                 { $$ = createReturn($2); }
    | RETURN ';'                    { $$ = createReturn(NULL); }
    ;


/* EXPRESSION RULES
 * Precedence and associativity come from the %left/%right table above, so
 * the grammar can stay one flat, readable `expr` rule. */
expr:
    NUM {
        $$ = createNum($1);
    }
    | ID {
        $$ = createVar($1);
        free($1);
    }
    /* DONE (Topic 3): two new primary expressions — an array element and a
     * function call used for its value. */
    | ID '[' expr ']' {
        $$ = createArrayIndex($1, $3);
        free($1);
    }
    | func_call {
        $$ = $1;
    }
    | expr '+' expr {
        $$ = createBinOp('+', $1, $3);
    }
    /* DONE (Topic 3): the rest of arithmetic.  No precedence is written into
     * these rules — the %left declarations already did that, so we do not
     * need term/factor layers. */
    | expr '-' expr {
        $$ = createBinOp('-', $1, $3);
    }
    | expr '*' expr {
        $$ = createBinOp('*', $1, $3);
    }
    | expr '/' expr {
        $$ = createBinOp('/', $1, $3);
    }
    /* DONE (Topic 3): unary minus.  %prec UMINUS stops  -2 * 3  parsing as
     * -(2 * 3).  Encoded as op 'u' with no right operand (see opText). */
    | '-' expr %prec UMINUS {
        $$ = createBinOp('u', $2, NULL);
    }
    /* DONE (Topic 3): parentheses.  No node for them: ( e ) just yields e.
     * The grouping they asked for is recorded in the tree's shape. */
    | '(' expr ')' {
        $$ = $2;
    }
    ;

/* ==========================================================================
 * FUNCTION CALLS AND THEIR ARGUMENTS                       DONE (Topic 3)
 *   func_call : ID '(' args ')'
 *   args      : (empty) | arg_list
 *   arg_list  : expr | arg_list ',' expr
 * (The handout also lists  ID '(' ')'  as a separate form; with an empty
 * `args` alternative that would be a reduce/reduce conflict, so the empty
 * case is handled by `args` alone.)
 * appendArg keeps arguments in SOURCE order so tac.c walks them left to
 * right — argument order is part of the calling convention.
 * ========================================================================*/
func_call:
    ID '(' args ')' {
        $$ = createFuncCall($1, $3);
        $$->lineno = @1.first_line;
        free($1);
    }
    ;

args:
    /* empty */                     { $$ = NULL; }
    | arg_list                      { $$ = $1; }
    ;

arg_list:
    expr                            { $$ = appendArg(NULL, $1); }
    | arg_list ',' expr             { $$ = appendArg($1, $3); }
    ;

/* PRINT STATEMENT */
print_stmt:
    PRINT '(' expr ')' ';' {
        $$ = createPrint($3);
    }
    | PRINT '(' expr ')' error {
        discardPendingError();
        fprintf(stderr, "Syntax Error at line %d: missing ';' after print statement\n",
                @4.first_line);
        $$ = NULL;
        YYABORT;
    }
    ;

%%

/* ERROR HANDLING */
/* yyerror: remember the message and the line (see DEFERRED SYNTAX ERRORS). */
void yyerror(const char* s) {
    snprintf(pendingError, sizeof pendingError, "%s", s);
    pendingLine = yylineno;
    hasPendingError = 1;
}

/* Print a syntax error that no error production claimed.  Called by main()
 * whenever yyparse() fails. */
void flushSyntaxError(void) {
    if (hasPendingError)
        fprintf(stderr, "Syntax Error at line %d: %s\n", pendingLine, pendingError);
    hasPendingError = 0;
}
