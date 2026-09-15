%{
/* ============================================================================
 *  TOPIC 1 · Compiler Design Phases
 * FILE: scanner.l   —   Phase 1: Lexical analysis
 * ----------------------------------------------------------------------------
 * PHASE:      Lexical analysis — the first of the six phases
 *             (scanner -> parser -> ast -> semantic -> tac -> codegen).
 * RECEIVES:   A raw character stream: the file main.c opened as yyin.  There
 *             is no previous phase; this is where the compiler meets the
 *             outside world.
 * PRODUCES:   A stream of (KIND, LEXEME, LINE, COLUMN) tokens, one per call
 *             to yylex(), plus a count of the characters the language does
 *             not admit.  In Topic 1 the consumer is main.c, which prints
 *             them; in Topic 2 the consumer is bison's yyparse(), which
 *             builds a syntax tree out of the same stream.
 * ----------------------------------------------------------------------------
 * Flex reads the rules below and writes a finite automaton that recognizes
 * all of them at once.  You never write the automaton; you write the
 * vocabulary, and two rules decide everything about how it behaves:
 *
 *   LONGEST MATCH   "<=" beats "<" because it consumes more characters.
 *                   Position in the file is irrelevant for this.
 *   FIRST MATCH     when two rules match the SAME number of characters, the
 *                   one written EARLIER wins.  This is the only reason the
 *                   keyword rules must appear above the identifier rule:
 *                   "int" matches both, both match three characters, and
 *                   whichever is written first decides the outcome.
 *
 * WHAT COMES NEXT
 *   In Topic 2 this same file feeds bison instead of main.c.  The actions
 *   change from "record a token for printing" to "return a token number";
 *   the patterns do not change at all.
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "tokens.h"

int lexErrorCount = 0;
int tokenCount    = 0;
int lineNo        = 1;
int colNo         = 1;

char lastLexeme[256];
int  lastLine;
int  lastCol;

/* Record where the token that is being returned started, then advance the
 * column past it.  Flex tracks lines for us but not columns, so we keep the
 * column by hand — and a column number is half of what makes an error
 * message actually findable.
 *
 * RETURNS: the token kind widened to int, because that is what yylex() is
 * declared to return and what bison will expect in Topic 2. */
static int tok(TokenKind k) {
    lastLine = lineNo;
    lastCol  = colNo;
    snprintf(lastLexeme, sizeof lastLexeme, "%s", yytext);
    colNo += (int)yyleng;
    tokenCount++;
    return (int)k;
}

/* Advance the position counters over text that produces no token at all:
 * whitespace, newlines, and the insides of comments. */
static void skip(const char* s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i] == '\n') { lineNo++; colNo = 1; }
        else if (s[i] == '\t') colNo += 4;
        else colNo++;
    }
}

/* Report one lexical error and KEEP GOING.
 *
 * WHY this does not exit: a scanner that stops at the first bad character
 * makes the programmer rebuild once per typo.  Every error the automaton can
 * still find after this one is a free diagnosis, so we report and resume.
 * The count is what main.c turns into a non-zero exit status at the end.
 *
 * WHY stderr: the token TABLE on stdout stays a clean artifact, so
 * `./lexer f.cm > table.txt` keeps the table and still shows the
 * diagnostics on the terminal.  stdout is flushed first so that the two
 * streams stay in order when both go to the same terminal. */
static void lexError(int line, int col, int ch) {
    lexErrorCount++;
    fflush(stdout);
    if (isprint(ch))
        fprintf(stderr,
                "  ✗ lexical error, line %d, column %d: "
                "character not in the language: '%c'\n", line, col, ch);
    else
        fprintf(stderr,
                "  ✗ lexical error, line %d, column %d: "
                "character not in the language: byte 0x%02X\n",
                line, col, (unsigned)ch);
}
%}

%option noyywrap
%option nounput
%option noinput

/* Named patterns keep the rules below readable.  A definition is pure
 * substitution — {digit}+ literally becomes [0-9]+ before flex sees it. */
digit   [0-9]
letter  [a-zA-Z_]
ident   {letter}({letter}|{digit})*
number  {digit}+

%%

 /* == 1 · WHITESPACE AND COMMENTS =========================================
  * These produce no token, but they are not free: every one of them moves
  * the position of every token after it.  Each action calls skip(), which is
  * what keeps lineNo and colNo honest across a comment that spans lines.
  * Drop the skip() call and tests/04_comments.cm reports `x = 1;` on line 4
  * instead of line 6.
  * ====================================================================== */

[ \t\r\n]+                      { skip(yytext, yyleng); }

 /* Line comment: two slashes to the end of the line.  The newline itself is
  * deliberately left for the whitespace rule above, so that exactly one rule
  * in this file ever increments lineNo. */
"//"[^\n]*                      { skip(yytext, yyleng); }

 /* Well-formed block comment, possibly spanning lines.
  * The body pattern reads as "anything that is not a star, or a run of stars
  * not followed by a slash" — precisely the set of strings that cannot
  * contain a closing delimiter.  Writing the body as .* instead would be
  * wrong: longest match would swallow one comment, the code between it and
  * the next comment, and the next comment, as a single match. */
"/*"([^*]|\*+[^*/])*\*+"/"      { skip(yytext, yyleng); }

 /* Unterminated block comment: an opening delimiter whose body runs to end
  * of file.  This rule can only win when the rule above fails, because
  * whenever a closing delimiter exists the well-formed rule matches strictly
  * more characters and longest match prefers it.
  *
  * It consumes the rest of the file on purpose.  Reporting the error and
  * then scanning the comment's prose as identifiers would bury one real
  * diagnostic under a pile of fictional tokens. */
"/*"([^*]|\*+[^*/])*\**         {
                                    lexErrorCount++;
                                    fflush(stdout);
                                    fprintf(stderr,
                                        "  ✗ lexical error, line %d, column %d: "
                                        "unterminated block comment, "
                                        "reached end of file\n", lineNo, colNo);
                                    skip(yytext, yyleng);
                                }

  /*  2. KEYWORDS — int print return if else while for switch case default
  *     break.  These must be written ABOVE the identifier rule.  Write them
  *     below it instead, run tests/03_keywords_vs_ids.cm, and see what
  *     happens; then move them and understand why it changed.
  *
  *  3. IDENTIFIERS AND NUMBERS — use the {ident} and {number} definitions
  *     already declared above.
  *
  *  4. TWO-CHARACTER OPERATORS — <= >= == != && ||
  *     Then the one-character ones: + - * / < > ! =
  *     Predict what happens if you omit the "<=" rule entirely, then try it.
  *
  *  5. DELIMITERS — ; : , ( ) { } [ ]
  *
  *  6. THE ERROR RULE — a single `.` pattern, written LAST.
  *     It must: report the offending character WITH ITS LINE AND COLUMN,
  *     increment lexErrorCount, advance colNo, and then CONTINUE SCANNING.
  *     Do not exit.  tests/05_lexical_errors.cm contains four bad characters
  *     and expects four messages from one run.
  *
  *  7. UNTERMINATED BLOCK COMMENT — a `/*` that reaches end of file.
  *     tests/06_unterminated_comment.cm expects exactly one error for this.
  *     Hint: a second pattern for "an opening /* followed by anything that
  *     never closes" will match only when the well-formed pattern cannot.
  *
  * WHEN YOU ARE DONE
  *   make test           must report 0 errors for tests 01-04
  *   ./lexer tests/05_lexical_errors.cm      must report 4 errors, exit 1
  *   ./lexer tests/06_unterminated_comment.cm must report 1 error,  exit 1
  * ══════════════════════════════════════════════════════════════════════ */

 /* TODO: write your rules here. */


%%
