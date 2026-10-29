/* =========================================================================
 * CST-405  ·  TOPIC 3  ·  Compiling Complex Variables and Functions
 * FILE: scanner.l   —   Phase 1 — Lexical analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *   ^^^^^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • Tokens for `[`, `]`, `,`, `{`, `}` and the arithmetic operators
 *   • `return` keyword for functions
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 *
 * STATUS
 *   Every TODO (Topic 3) in this file has been implemented.  Lexical errors
 *   are now counted (lexErrors) so main.c can fail the compile.
 * ========================================================================= */

%{
/* PHASE 1 — LEXICAL ANALYSIS
 *
 * The scanner turns a stream of CHARACTERS into a stream of TOKENS.
 * It is the only part of the compiler that ever sees whitespace or comments.
 *
 *      int  sum  =  a + 10 ;
 *      └─┬┘ └─┬┘ │  │ │ └┬┘ │
 *       INT  ID  '=' ID '+' NUM ';'
 *
 * Flex turns the rules below into a finite automaton.  Two rules decide
 * everything you need to know about how it behaves:
 *
 *   LONGEST MATCH   "<=" wins over "<" because it matches more characters.
 *                   This is why "<=" must exist as a rule at all, and why
 *                   its position in the file does not matter.
 *   FIRST MATCH     when two rules match the SAME length, the one written
 *                   first wins.  This is why the keyword rules sit above
 *                   the identifier rule: without that order, "int" would
 *                   come back as an ID.
 */
#include <stdio.h>
#include <stdlib.h>
#include "parser.tab.h"  /* Token definitions from Bison */

/* Count of lexical errors.  main.c fails the compile when this is non-zero,
 * so a stray character is a real error and not just a printed warning. */
int lexErrors = 0;

/* Record each token's line in yylloc so the parser's @n locations work.
 * Error productions use them to report a missing ';' on the line that is
 * actually missing it, not the line where the parser noticed. */
#define YY_USER_ACTION  yylloc.first_line = yylloc.last_line = yylineno;
%}

/* Flex options to suppress warnings for unused functions */
%option nounput
%option noinput
%option yylineno

%%

"int"           { return INT; }
"print"         { return PRINT; }
"return"        { return RETURN; }   /* Topic 3: functions return values.
                                      * Sits ABOVE the ID rule: same length
                                      * match, so first-match makes it a
                                      * keyword rather than an identifier. */

[a-zA-Z_][a-zA-Z0-9_]* {
    yylval.str = strdup(yytext);
    return ID;
}

[0-9]+          {
    yylval.num = atoi(yytext);
    return NUM;
}


"+"             { return '+'; }
"-"             { return '-'; }   /* binary minus AND unary minus: the
                                   * parser tells them apart, not us     */
"*"             { return '*'; }
"/"             { return '/'; }   /* Never eats a comment: "//" and "/ *"
                                   * match 2+ characters, so LONGEST MATCH
                                   * picks the comment rules over this one */


"="             { return '='; }

";"             { return ';'; }
"("             { return '('; }
")"             { return ')'; }
"{"             { return '{'; }   /* blocks and function bodies          */
"}"             { return '}'; }
"["             { return '['; }   /* array declaration and indexing      */
"]"             { return ']'; }
","             { return ','; }   /* parameter and argument lists        */

"//".*          { /* Skip single-line comments */ }
"/*"([^*]|\*+[^*/])*\*+"/"  { /* Skip multi-line comments */ }

[ \t\r\n]       { /* whitespace (\r too, for files saved on Windows) */ }

.               {
                    fprintf(stderr, "Lexical Error at line %d: unknown character '%c'\n",
                            yylineno, *yytext);
                    lexErrors++;   /* keep scanning so every bad char is reported */
                }

%%

int yywrap() {
    return 1;
}
