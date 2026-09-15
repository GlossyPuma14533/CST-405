/* ============================================================================
 *  TOPIC 1 · Compiler Design Phases
 * FILE: main.c   —   the driver for Phase 1
 *
 * PHASE:    Driver for phase 1 (lexical analysis).  It implements no phase of
 *           its own; it is the temporary consumer that stands where the
 *           parser will stand in Topic 2.
 * RECEIVES: Tokens from the scanner in scanner.l — one (KIND, LEXEME, LINE,
 *           COLUMN) per call to yylex() — plus the counters that file keeps.
 * PRODUCES: A token table on stdout, a summary, and an exit status.  In
 *           Topic 2 this consumer is replaced by bison's yyparse(), which
 *           reads exactly the same stream and builds a syntax tree from it.
 * ----------------------------------------------------------------------------
 * This program is a compiler with exactly one phase in it.  It reads a source
 * file, asks the scanner for tokens until there are none left, and prints
 * what it got.  That is all a lexical analyzer ever does.
 *
 * The output is deliberately a TABLE rather than a running commentary,
 * because the table is the thing you hand to the parser in Topic 2 and the
 * thing you point at in your video when you say "here is what my scanner
 * produces for this input".
 *
 *   ./lexer program.cm          list every token
 *   ./lexer program.cm -c       count only (no listing)
 *
 * EXIT STATUS
 *   0   no lexical errors — the source is certified lexically correct
 *   1   at least one lexical error, all of them reported above
 *   2   the file could not be opened
 *
 * A non-zero exit status matters: it is what lets `make` and a test script
 * tell success from failure without reading the output.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tokens.h"

/* --------------------------------------------------------------------------
 * The printable name of each token kind.  Keeping this next to the enum in
 * tokens.h is a maintenance trap — add a kind there and forget it here and
 * the table silently prints "?".  A real compiler solves this with an X-macro;
 * doing that is a good optional exercise.
 * -------------------------------------------------------------------------*/
const char* tokenName(TokenKind k) {
    /* One case per kind, deliberately with no `default:` label.  A default
     * would silence the one diagnostic that keeps this function in step with
     * the enum: -Wall warns about a switch over an enum with a missing case,
     * so adding a kind to tokens.h and forgetting it here is a build-time
     * warning rather than a "?" nobody notices in the table. */
    switch (k) {
        case TOK_EOF:      return "EOF";

        case TOK_INT:      return "INT";
        case TOK_PRINT:    return "PRINT";
        case TOK_RETURN:   return "RETURN";
        case TOK_IF:       return "IF";
        case TOK_ELSE:     return "ELSE";
        case TOK_WHILE:    return "WHILE";
        case TOK_FOR:      return "FOR";
        case TOK_SWITCH:   return "SWITCH";
        case TOK_CASE:     return "CASE";
        case TOK_DEFAULT:  return "DEFAULT";
        case TOK_BREAK:    return "BREAK";

        case TOK_ID:       return "ID";
        case TOK_NUM:      return "NUM";

        case TOK_PLUS:     return "PLUS";
        case TOK_MINUS:    return "MINUS";
        case TOK_STAR:     return "STAR";
        case TOK_SLASH:    return "SLASH";
        case TOK_LT:       return "LT";
        case TOK_GT:       return "GT";
        case TOK_LE:       return "LE";
        case TOK_GE:       return "GE";
        case TOK_EQ:       return "EQ";
        case TOK_NE:       return "NE";
        case TOK_AND:      return "AND";
        case TOK_OR:       return "OR";
        case TOK_NOT:      return "NOT";
        case TOK_ASSIGN:   return "ASSIGN";

        case TOK_SEMI:     return "SEMI";
        case TOK_COLON:    return "COLON";
        case TOK_COMMA:    return "COMMA";
        case TOK_LPAREN:   return "LPAREN";
        case TOK_RPAREN:   return "RPAREN";
        case TOK_LBRACE:   return "LBRACE";
        case TOK_RBRACE:   return "RBRACE";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";

        case TOK_ERROR:    return "ERROR";
    }
    /* Unreachable for any declared kind; reached only if a caller casts an
     * arbitrary int to TokenKind, which nothing in this program does. */
    return "?";
}

const char* tokenCategory(TokenKind k) {
    /* What the kind IS, so the table explains itself rather than merely
     * reporting.  Thirty-odd kinds collapse into eight answers, which is the
     * point: the categories are the grouping the PARSER cares about in Topic
     * 2, where precedence and associativity are declared per category, not
     * per token.  Cases are grouped (fallthrough between labels with no
     * statements between them) so the grouping is visible in the source. */
    switch (k) {
        case TOK_INT: case TOK_PRINT: case TOK_RETURN:
        case TOK_IF:  case TOK_ELSE:  case TOK_WHILE: case TOK_FOR:
        case TOK_SWITCH: case TOK_CASE: case TOK_DEFAULT: case TOK_BREAK:
            return "keyword";

        case TOK_ID:
            return "identifier";

        case TOK_NUM:
            return "integer literal";

        case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH:
            return "arithmetic operator";

        case TOK_LT: case TOK_GT: case TOK_LE:
        case TOK_GE: case TOK_EQ: case TOK_NE:
            return "relational operator";

        case TOK_AND: case TOK_OR: case TOK_NOT:
            return "logical operator";

        case TOK_ASSIGN:
            return "assignment";

        case TOK_SEMI: case TOK_COLON: case TOK_COMMA:
        case TOK_LPAREN: case TOK_RPAREN:
        case TOK_LBRACE: case TOK_RBRACE:
        case TOK_LBRACKET: case TOK_RBRACKET:
            return "delimiter";

        /* Neither of these ever reaches the table: yylex() returning 0 ends
         * the loop in main(), and the scanner's error rule reports and
         * resumes instead of returning TOK_ERROR.  They are listed so the
         * switch stays exhaustive and -Wall keeps watching it. */
        case TOK_EOF:      return "end of file";
        case TOK_ERROR:    return "lexical error";
    }
    return "?";
}

int main(int argc, char** argv) {
    int countOnly = 0;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <source-file> [-c]\n", argv[0]);
        fprintf(stderr, "  -c   print only the summary, not the token table\n");
        return 2;
    }
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "-c") == 0) countOnly = 1;

    yyin = fopen(argv[1], "r");
    if (!yyin) {
        fprintf(stderr, "Error: cannot open '%s'\n", argv[1]);
        return 2;
    }

    printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  PHASE 1 · LEXICAL ANALYSIS                            ║\n");
    printf("║  source: %-55s ║\n", argv[1]);
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    if (!countOnly) {
        printf("   #  LINE  COL  TOKEN        LEXEME            KIND\n");
        printf(" ───  ────  ───  ───────────  ────────────────  ─────────────────────\n");
    }

    int n = 0;
    int kind;
    while ((kind = yylex()) != 0) {
        n++;
        if (!countOnly)
            printf(" %3d  %4d  %3d  %-11s  %-16.16s  %s\n",
                   n, lastLine, lastCol,
                   tokenName((TokenKind)kind), lastLexeme,
                   tokenCategory((TokenKind)kind));
    }

    fclose(yyin);

    printf("\n───────────────────────────────────────────────────────────────────\n");
    printf("  Tokens recognized : %d\n", tokenCount);
    printf("  Lines read        : %d\n", lineNo);
    printf("  Lexical errors    : %d\n", lexErrorCount);
    printf("───────────────────────────────────────────────────────────────────\n");

    if (tokenCount == 0) {
        printf("\n  ⚠  NO TOKENS RECOGNISED\n");
        printf("     The scanner read the file and produced nothing.  That is\n");
        printf("     not a certificate of correctness — it means no rule in\n");
        printf("     scanner.l matched anything yet.\n\n");
        return 1;
    }

    if (lexErrorCount == 0) {
        printf("\n  ✓ CERTIFIED LEXICALLY CORRECT\n");
        printf("    Every character in this file belongs to the language.\n");
        printf("    That is a claim about VOCABULARY only — this program says\n");
        printf("    nothing about whether the tokens form a legal sentence.\n");
        printf("    Answering that is the parser's job, in Topic 2.\n\n");
        return 0;
    }

    printf("\n  ✗ %d LEXICAL ERROR(S)\n", lexErrorCount);
    printf("    Each one is reported above with its line and column.\n");
    printf("    Scanning continued past every error on purpose, so that one\n");
    printf("    run reports every problem rather than only the first.\n\n");
    return 1;
}
