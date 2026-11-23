/* =========================================================================
 * CST-405  ·  TOPIC 4  ·  Compiling Loops
 * FILE: scanner.l   —   Phase 1 — Lexical analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *   ^^^^^^^  this file
 *
 * WHAT IS NEW IN TOPIC 4
 *   • Relational operators `<  >  <=  >=  ==  !=`
 *   • Keywords `while`, `for`, `break`
 *
 * WHAT COMES NEXT
 *   Topic 5 adds decisions (if, if-else, switch) and the logical operators.
 *
 * STATUS
 *   Topic 4 loop tokens are implemented.  Project 4 also restores our
 *   Project 3 work: lexical errors are counted (lexErrors) so main.c fails
 *   the compile, \r is whitespace, and yylloc feeds the parser's @n lines.
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

/* Count of lexical errors (restored from our Project 3 work).  main.c fails
 * the compile when this is non-zero, so a stray character is a real error
 * and not just a printed warning that still produces assembly. */
int lexErrors = 0;

/* Record each token's line in yylloc so the parser's @n locations work.
 * The error productions in parser.y use them to report a missing ';' on the
 * line that is actually missing it, not the line where the parser noticed. */
#define YY_USER_ACTION  yylloc.first_line = yylloc.last_line = yylineno;
%}

/* Flex options to suppress warnings for unused functions */
%option nounput
%option noinput
%option yylineno

%%

"int"           { return INT; }
"print"         { return PRINT; }
"return"        { return RETURN; }
    /* Loop keywords.  They sit above the identifier rule on purpose: flex
     * picks the longest match and, on a tie, the EARLIER rule, so "while"
     * ties with the ID pattern and the keyword wins only by coming first. */
"while"         { return WHILE; }
"for"           { return FOR; }
"break"         { return BREAK; }

[a-zA-Z_][a-zA-Z0-9_]* {
    yylval.str = strdup(yytext);
    return ID;
}

[0-9]+          {
    yylval.num = atoi(yytext);
    return NUM;
}


    /* Two-character relational operators.  These MUST be their own rules:
     * without a "<=" rule flex would match "<" and then "=" and the parser
     * would see two tokens.  Longest-match makes "<=" beat "<" automatically. */
"<="            { return LE; }
">="            { return GE; }
"=="            { return EQ; }
"!="            { return NE; }

"+"             { return '+'; }
"-"             { return '-'; }
"*"             { return '*'; }
"/"             { return '/'; }

    /* Single-character relational operators (returned as themselves). */
"<"             { return '<'; }
">"             { return '>'; }

"="             { return '='; }

";"             { return ';'; }
"("             { return '('; }
")"             { return ')'; }
"{"             { return '{'; }
"}"             { return '}'; }
"["             { return '['; }
"]"             { return ']'; }
","             { return ','; }

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
