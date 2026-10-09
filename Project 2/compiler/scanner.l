/* =========================================================================
 * CST-405  ·  TOPIC 2  ·  Compiler for a Starter Language
 * FILE: scanner.l   —   Phase 1 — Lexical analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *   ^^^^^^^  this file
 *
 *   RECEIVES : the source file, as raw characters (yyin)
 *   PRODUCES : a stream of tokens, one per yylex() call, with yylval/yylloc filled in
 *
 * WHAT IS NEW IN TOPIC 2
 *   • The starter token set: `int`, `print`, identifiers, numbers,
 *   • `+`, `=`, `;`, `(`, `)`, and both comment forms
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
#include "trace.h"       /* trace(): silent under -q */

/* Number of lexical errors seen.  main() checks it after parsing: a bad
 * character is SKIPPED so scanning can continue and report every error in
 * one run, which means the parser may still succeed — so the compile must be
 * failed explicitly here, or a program containing '@' would compile. */
int lexErrorCount = 0;

/* TOKEN STREAM DISPLAY
 * Every token is echoed (line, token kind, lexeme) as it is handed to the
 * parser, so Phase 1's output can be inspected on its own.  Tokens appear in
 * the order bison REQUESTS them — one token of lookahead at a time. */
/* Give the parser each token's line number for its @n locations. */
#define YY_USER_ACTION  yylloc.first_line = yylloc.last_line = yylineno;

#define TOKEN(kind, name) \
    do { trace("    line %-3d %-6s '%s'\n", yylineno, name, yytext); return kind; } while (0)
%}

/* Flex options to suppress warnings for unused functions */
%option nounput
%option noinput
%option yylineno

%%

"int"           { TOKEN(INT,   "INT"); }
"print"         { TOKEN(PRINT, "PRINT"); }

[a-zA-Z_][a-zA-Z0-9_]* {
    yylval.str = strdup(yytext);   /* the parser rule that receives it frees it */
    TOKEN(ID, "ID");
}

[0-9]+          {
    /* KNOWN LIMITATION: atoi() does not detect overflow, so a literal
     * larger than 2147483647 silently wraps instead of being reported. */
    yylval.num = atoi(yytext);
    TOKEN(NUM, "NUM");
}

"+"             { TOKEN('+', "'+'"); }
"="             { TOKEN('=', "'='"); }
";"             { TOKEN(';', "';'"); }
"("             { TOKEN('(', "'('"); }
")"             { TOKEN(')', "')'"); }

"//".*          { /* Skip single-line comments */ }
"/*"([^*]|\*+[^*/])*\*+"/"  { /* Skip multi-line comments */ }

[ \t\r\n]       { /* whitespace (\r too, so Windows line endings scan cleanly) */ }

.               {
                    /* Any character no rule above accepts.  Report it with its
                     * line, skip it, and keep scanning so every lexical error
                     * in the file is reported in one run. */
                    fprintf(stderr, "Lexical Error at line %d: unknown character '%c'\n",
                            yylineno, *yytext);
                    lexErrorCount++;
                }

%%

int yywrap() {
    return 1;
}
