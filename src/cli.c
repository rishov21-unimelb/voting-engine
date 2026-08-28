/*
 * cli.c -- argument handling, terminal rendering and result export.
 *
 * This is the only module that knows about terminals, colours and file
 * formats on the way out; the engine below it deals purely in data.
 *
 * SPDX-License-Identifier: MIT
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "voting.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#define VOTE_ISATTY(stream) (_isatty(_fileno(stream)) != 0)
#else
#include <unistd.h>
#define VOTE_ISATTY(stream) (isatty(fileno(stream)) != 0)
#endif

/* ------------------------------------------------------------------ */
/* Presentation helpers                                                */
/* ------------------------------------------------------------------ */

#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_DIM    "\033[2m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_RED    "\033[31m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_YELLOW "\033[33m"

#define BAR_WIDTH 24

/* Returns the escape sequence when colour is on, and "" when it is off,
 * so format strings stay readable at the call site. */
static const char *paint(bool color, const char *seq)
{
    return color ? seq : "";
}

static void print_bar(FILE *out, double fraction, bool color)
{
    int filled, i;

    if (fraction < 0.0) {
        fraction = 0.0;
    }
    if (fraction > 1.0) {
        fraction = 1.0;
    }
    filled = (int)(fraction * BAR_WIDTH + 0.5);
    fputs(paint(color, ANSI_CYAN), out);
    for (i = 0; i < BAR_WIDTH; ++i) {
        fputc(i < filled ? '#' : '.', out);
    }
    fputs(paint(color, ANSI_RESET), out);
}

/* Widest candidate name, so the table columns line up whatever the
 * input contains. */
static int name_column(const Election *e)
{
    size_t i, widest = 8;

    for (i = 0; i < e->candidates.count; ++i) {
        size_t len = strlen(e->candidates.items[i].name);
        if (len > widest) {
            widest = len;
        }
    }
    if (widest > 32u) {
        widest = 32u; /* keep pathological names from wrecking the table */
    }
    return (int)widest;
}

static const char *unit_for(VotingSystem system)
{
    return (system == VOTE_SYSTEM_BORDA) ? "points" : "votes";
}

/* ------------------------------------------------------------------ */
/* Terminal report                                                     */
/* ------------------------------------------------------------------ */

void vote_report(FILE *out, const Election *e, const ElectionResult *r,
                 const ReportOptions *opt)
{
    ReportOptions defaults = { false, false, true };
    size_t        i, j;
    int           width;

    if (out == NULL || e == NULL || r == NULL) {
        return;
    }
    if (opt == NULL) {
        opt = &defaults;
    }
    width = name_column(e);

    fprintf(out, "%sElection%s\n", paint(opt->color, ANSI_BOLD),
            paint(opt->color, ANSI_RESET));
    fprintf(out, "  candidates : %lu\n",
            (unsigned long)vote_candidate_count(e));
    fprintf(out, "  ballots    : %lu\n", (unsigned long)r->ballot_count);
    fprintf(out, "  system     : %s\n", vote_system_name(r->system));

    if (!opt->quiet) {
        for (i = 0; i < r->count; ++i) {
            const ElectionStage *stage = &r->rounds[i];

            fprintf(out, "\n%sRound %lu%s\n", paint(opt->color, ANSI_BOLD),
                    (unsigned long)stage->round, paint(opt->color, ANSI_RESET));

            for (j = 0; j < stage->count; ++j) {
                const TallyEntry *entry = &stage->entries[j];
                fprintf(out, "  %-*.*s %8.0f %-6s %5.1f%%  ", width, width,
                        entry->name, entry->score, unit_for(r->system),
                        entry->percentage);
                print_bar(out, entry->percentage / 100.0, opt->color);
                fputc('\n', out);
            }
            if (stage->exhausted > 0) {
                fprintf(out, "  %s(%lu exhausted)%s\n",
                        paint(opt->color, ANSI_DIM),
                        (unsigned long)stage->exhausted,
                        paint(opt->color, ANSI_RESET));
            }
            fprintf(out, "  ----\n");
            if (stage->decided) {
                const Candidate *c = vote_candidate_at(e, stage->winner);
                fprintf(out, "  %s%s is declared elected%s\n",
                        paint(opt->color, ANSI_GREEN),
                        (c != NULL) ? c->name : "?",
                        paint(opt->color, ANSI_RESET));
            } else if (stage->eliminated_any) {
                const Candidate *c = vote_candidate_at(e, stage->eliminated);
                fprintf(out, "  %s%s is eliminated%s, votes distributed%s\n",
                        paint(opt->color, ANSI_RED),
                        (c != NULL) ? c->name : "?",
                        paint(opt->color, ANSI_RESET),
                        stage->tie_broken ? " (tie broken)" : "");
            } else {
                fprintf(out, "  no candidate elected\n");
            }
        }
    }

    if (opt->summary) {
        fprintf(out, "\n%sResult%s\n", paint(opt->color, ANSI_BOLD),
                paint(opt->color, ANSI_RESET));
        if (r->decided && r->count > 0) {
            const ElectionStage *last = &r->rounds[r->count - 1u];
            const Candidate     *c = vote_candidate_at(e, r->winner);
            double               score = 0.0, pct = 0.0;

            for (j = 0; j < last->count; ++j) {
                if (last->entries[j].candidate == r->winner) {
                    score = last->entries[j].score;
                    pct   = last->entries[j].percentage;
                    break;
                }
            }
            fprintf(out, "  %s%s%s elected in round %lu with %.0f %s (%.1f%%)\n",
                    paint(opt->color, ANSI_GREEN),
                    (c != NULL) ? c->name : "?", paint(opt->color, ANSI_RESET),
                    (unsigned long)last->round, score, unit_for(r->system),
                    pct);
        } else {
            fprintf(out, "  %sno candidate could be elected%s\n",
                    paint(opt->color, ANSI_YELLOW),
                    paint(opt->color, ANSI_RESET));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Export                                                              */
/* ------------------------------------------------------------------ */

/* Write `s` as a JSON string literal, escaping what the grammar
 * requires and passing UTF-8 bytes through untouched. */
static void json_write_string(FILE *out, const char *s)
{
    fputc('"', out);
    for (; *s != '\0'; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\b': fputs("\\b", out);  break;
        case '\f': fputs("\\f", out);  break;
        case '\n': fputs("\\n", out);  break;
        case '\r': fputs("\\r", out);  break;
        case '\t': fputs("\\t", out);  break;
        default:
            if (c < 0x20u) {
                fprintf(out, "\\u%04x", (unsigned)c);
            } else {
                fputc((int)c, out);
            }
            break;
        }
    }
    fputc('"', out);
}

static void json_write_result(FILE *out, const Election *e,
                              const ElectionResult *r)
{
    size_t i, j;

    fprintf(out, "{\n");
    fprintf(out, "  \"generator\": \"voting-engine %s\",\n",
            VOTING_VERSION_STRING);
    fprintf(out, "  \"system\": \"%s\",\n", vote_system_name(r->system));
    fprintf(out, "  \"ballots\": %lu,\n", (unsigned long)r->ballot_count);

    fprintf(out, "  \"candidates\": [");
    for (i = 0; i < e->candidates.count; ++i) {
        if (i > 0) {
            fputs(", ", out);
        }
        json_write_string(out, e->candidates.items[i].name);
    }
    fprintf(out, "],\n");

    fprintf(out, "  \"decided\": %s,\n", r->decided ? "true" : "false");
    fputs("  \"winner\": ", out);
    if (r->decided) {
        const Candidate *c = vote_candidate_at(e, r->winner);
        json_write_string(out, (c != NULL) ? c->name : "");
    } else {
        fputs("null", out);
    }
    fputs(",\n  \"rounds\": [\n", out);

    for (i = 0; i < r->count; ++i) {
        const ElectionStage *stage = &r->rounds[i];

        fprintf(out, "    {\n");
        fprintf(out, "      \"round\": %lu,\n", (unsigned long)stage->round);
        fprintf(out, "      \"total\": %.0f,\n", stage->total);
        fprintf(out, "      \"exhausted\": %lu,\n",
                (unsigned long)stage->exhausted);
        fprintf(out, "      \"tally\": [\n");
        for (j = 0; j < stage->count; ++j) {
            const TallyEntry *entry = &stage->entries[j];
            fputs("        {\"candidate\": ", out);
            json_write_string(out, entry->name);
            fprintf(out, ", \"score\": %.0f, \"share\": %.4f}%s\n",
                    entry->score, entry->percentage / 100.0,
                    (j + 1u < stage->count) ? "," : "");
        }
        fprintf(out, "      ],\n");

        fputs("      \"eliminated\": ", out);
        if (stage->eliminated_any) {
            const Candidate *c = vote_candidate_at(e, stage->eliminated);
            json_write_string(out, (c != NULL) ? c->name : "");
        } else {
            fputs("null", out);
        }
        fprintf(out, ",\n      \"tie_broken\": %s,\n",
                stage->tie_broken ? "true" : "false");

        fputs("      \"elected\": ", out);
        if (stage->decided) {
            const Candidate *c = vote_candidate_at(e, stage->winner);
            json_write_string(out, (c != NULL) ? c->name : "");
        } else {
            fputs("null", out);
        }
        fprintf(out, "\n    }%s\n", (i + 1u < r->count) ? "," : "");
    }
    fprintf(out, "  ]\n}\n");
}

/* Open `path` for writing, or hand back stdout when it is "-". */
static FILE *open_sink(const char *path, bool *is_stdout)
{
    if (path == NULL) {
        return NULL;
    }
    if (strcmp(path, "-") == 0) {
        *is_stdout = true;
        return stdout;
    }
    *is_stdout = false;
    return fopen(path, "w");
}

vote_status_t vote_export_json(const char *path, const Election *e,
                               const ElectionResult *r)
{
    bool  to_stdout = false;
    FILE *out;

    if (path == NULL || e == NULL || r == NULL) {
        return VOTE_ERR_INVALID;
    }
    out = open_sink(path, &to_stdout);
    if (out == NULL) {
        return VOTE_ERR_IO;
    }
    json_write_result(out, e, r);
    if (!to_stdout) {
        if (fclose(out) != 0) {
            return VOTE_ERR_IO;
        }
    }
    return VOTE_OK;
}

vote_status_t vote_export_text(const char *path, const Election *e,
                               const ElectionResult *r)
{
    ReportOptions opt = { false, false, true };
    bool          to_stdout = false;
    FILE         *out;

    if (path == NULL || e == NULL || r == NULL) {
        return VOTE_ERR_INVALID;
    }
    out = open_sink(path, &to_stdout);
    if (out == NULL) {
        return VOTE_ERR_IO;
    }
    vote_report(out, e, r, &opt);
    if (!to_stdout) {
        if (fclose(out) != 0) {
            return VOTE_ERR_IO;
        }
    }
    return VOTE_OK;
}

/* ------------------------------------------------------------------ */
/* Interactive entry                                                   */
/* ------------------------------------------------------------------ */

/* Read one line from stdin into a growable buffer; returns false at
 * EOF. The trailing newline is stripped. */
static bool prompt_line(char **buf, size_t *cap)
{
    size_t len = 0;
    char  *grown;
    int    c;

    for (;;) {
        c = fgetc(stdin);
        if (c == EOF) {
            if (len == 0) {
                return false;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        grown = vote_reserve(*buf, cap, len + 2u, sizeof **buf);
        if (grown == NULL) {
            return false;
        }
        *buf = grown;
        (*buf)[len++] = (char)c;
    }
    grown = vote_reserve(*buf, cap, len + 1u, sizeof **buf);
    if (grown == NULL) {
        return false;
    }
    *buf = grown;
    if (len > 0 && (*buf)[len - 1u] == '\r') {
        len--;
    }
    (*buf)[len] = '\0';
    return true;
}

static bool line_is_blank(const char *s)
{
    for (; *s != '\0'; ++s) {
        if (*s != ' ' && *s != '\t') {
            return false;
        }
    }
    return true;
}

/* Guided session for someone typing at a terminal rather than piping a
 * file in. Candidate names first, then one ballot per line. */
static vote_status_t run_interactive(Election *e)
{
    char         *buf = NULL;
    size_t        cap = 0;
    unsigned     *ranks = NULL;
    size_t        n;
    vote_status_t status = VOTE_OK;

    fputs("Enter candidate names, one per line. Blank line when done.\n", stdout);
    for (;;) {
        fprintf(stdout, "  candidate %lu> ",
                (unsigned long)(vote_candidate_count(e) + 1u));
        fflush(stdout);
        if (!prompt_line(&buf, &cap)) {
            break;
        }
        if (line_is_blank(buf)) {
            break;
        }
        status = vote_candidate_add(e, buf, NULL);
        if (status != VOTE_OK) {
            fprintf(stdout, "  ! %s\n", vote_status_str(status));
            status = VOTE_OK; /* let the user try again */
        }
    }
    n = vote_candidate_count(e);
    if (n < 2u) {
        free(buf);
        return VOTE_ERR_EMPTY;
    }

    ranks = malloc(n * sizeof *ranks);
    if (ranks == NULL) {
        free(buf);
        return VOTE_ERR_NOMEM;
    }
    fprintf(stdout,
            "\nEnter one ballot per line: %lu ranks, in candidate order.\n"
            "Example: \"1 2 3\" ranks the first candidate highest. "
            "Blank line when done.\n",
            (unsigned long)n);
    for (;;) {
        char  *cursor;
        size_t seen = 0;

        fprintf(stdout, "  ballot %lu> ",
                (unsigned long)(vote_ballot_count(e) + 1u));
        fflush(stdout);
        if (!prompt_line(&buf, &cap)) {
            break;
        }
        if (line_is_blank(buf)) {
            break;
        }
        cursor = buf;
        while (seen < n) {
            char         *stop = NULL;
            unsigned long value;

            while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
                ++cursor;
            }
            if (*cursor == '\0') {
                break;
            }
            value = strtoul(cursor, &stop, 10);
            if (stop == cursor || value > n) {
                seen = 0;
                break;
            }
            ranks[seen++] = (unsigned)value;
            cursor = stop;
        }
        if (seen != n) {
            fprintf(stdout, "  ! expected %lu ranks\n", (unsigned long)n);
            continue;
        }
        status = vote_ballot_add(e, ranks, n);
        if (status != VOTE_OK) {
            fprintf(stdout, "  ! %s\n", vote_status_str(status));
            status = VOTE_OK;
        }
    }

    free(ranks);
    free(buf);
    return (vote_ballot_count(e) == 0) ? VOTE_ERR_EMPTY : VOTE_OK;
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "voting-engine %s -- preferential election counting\n"
        "\n"
        "Usage: %s [OPTIONS] [FILE]\n"
        "\n"
        "Reads an election from FILE, or from standard input when FILE is\n"
        "omitted or given as \"-\".\n"
        "\n"
        "Options:\n"
        "  -s, --system=NAME   instant-runoff (default), borda, plurality\n"
        "  -f, --format=NAME   auto (default), text, csv, json\n"
        "  -e, --export=FILE   write the outcome to FILE (.json for JSON,\n"
        "                      otherwise plain text; \"-\" for stdout)\n"
        "      --color=WHEN    auto (default), always, never\n"
        "  -i, --interactive   prompt for candidates and ballots\n"
        "  -q, --quiet         print only the final result\n"
        "  -h, --help          show this help and exit\n"
        "  -V, --version       show the version and exit\n"
        "\n"
        "Examples:\n"
        "  %s examples/simple.txt\n"
        "  %s --system=borda examples/election.csv\n"
        "  %s --system=plurality --export=results.json examples/election.json\n"
        "  cat ballots.txt | %s --quiet\n",
        VOTING_VERSION_STRING, prog, prog, prog, prog, prog);
}

/* Match "--name=value" or "--name value", advancing *i for the latter.
 * Returns NULL when `arg` is a different option. */
static const char *option_value(const char *arg, const char *name,
                                char **argv, int argc, int *i)
{
    size_t len = strlen(name);

    if (strncmp(arg, name, len) != 0) {
        return NULL;
    }
    if (arg[len] == '=') {
        return arg + len + 1;
    }
    if (arg[len] == '\0') {
        if (*i + 1 < argc) {
            (*i)++;
            return argv[*i];
        }
        return NULL;
    }
    return NULL;
}

typedef struct {
    const char  *input;
    const char  *export_path;
    VotingSystem system;
    VoteFormat   format;
    bool         interactive;
    bool         quiet;
    int          color_mode; /* -1 never, 0 auto, 1 always */
} CliOptions;

/* Returns 0 to continue, 1 to exit successfully, 2 on a usage error. */
static int parse_args(int argc, char **argv, CliOptions *o)
{
    const char *prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "vote";
    int         i;

    o->input       = NULL;
    o->export_path = NULL;
    o->system      = VOTE_SYSTEM_INSTANT_RUNOFF;
    o->format      = VOTE_FORMAT_AUTO;
    o->interactive = false;
    o->quiet       = false;
    o->color_mode  = 0;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout, prog);
            return 1;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("voting-engine %s\n", VOTING_VERSION_STRING);
            return 1;
        }
        if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
            o->quiet = true;
            continue;
        }
        if (strcmp(arg, "-i") == 0 || strcmp(arg, "--interactive") == 0) {
            o->interactive = true;
            continue;
        }
        if (strcmp(arg, "--no-color") == 0) {
            o->color_mode = -1;
            continue;
        }
        if ((value = option_value(arg, "--system", argv, argc, &i)) != NULL ||
            (value = option_value(arg, "-s", argv, argc, &i)) != NULL) {
            if (vote_system_parse(value, &o->system) != VOTE_OK) {
                fprintf(stderr, "%s: unknown voting system '%s'\n", prog, value);
                return 2;
            }
            continue;
        }
        if ((value = option_value(arg, "--format", argv, argc, &i)) != NULL ||
            (value = option_value(arg, "-f", argv, argc, &i)) != NULL) {
            if (vote_format_parse(value, &o->format) != VOTE_OK) {
                fprintf(stderr, "%s: unknown input format '%s'\n", prog, value);
                return 2;
            }
            continue;
        }
        if ((value = option_value(arg, "--export", argv, argc, &i)) != NULL ||
            (value = option_value(arg, "-e", argv, argc, &i)) != NULL) {
            o->export_path = value;
            continue;
        }
        if ((value = option_value(arg, "--color", argv, argc, &i)) != NULL) {
            if (vote_ascii_casecmp(value, "always") == 0) {
                o->color_mode = 1;
            } else if (vote_ascii_casecmp(value, "never") == 0) {
                o->color_mode = -1;
            } else if (vote_ascii_casecmp(value, "auto") == 0) {
                o->color_mode = 0;
            } else {
                fprintf(stderr, "%s: unknown color mode '%s'\n", prog, value);
                return 2;
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0' && strcmp(arg, "-") != 0) {
            fprintf(stderr, "%s: unknown option '%s'\n", prog, arg);
            fprintf(stderr, "Try '%s --help' for more information.\n", prog);
            return 2;
        }
        if (o->input != NULL) {
            fprintf(stderr, "%s: more than one input file given\n", prog);
            return 2;
        }
        o->input = arg;
    }
    return 0;
}

static bool want_color(int mode)
{
    if (mode > 0) {
        return true;
    }
    if (mode < 0) {
        return false;
    }
    /* https://no-color.org: any value at all disables colour. */
    if (getenv("NO_COLOR") != NULL) {
        return false;
    }
    return VOTE_ISATTY(stdout);
}

int vote_cli_main(int argc, char **argv)
{
    const char    *prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "vote";
    CliOptions     opts;
    Election       election;
    ElectionResult result;
    ReportOptions  report;
    vote_status_t  status;
    size_t         err_line = 0;
    int            rc;

    rc = parse_args(argc, argv, &opts);
    if (rc != 0) {
        return (rc == 1) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    vote_election_init(&election);

    if (opts.interactive) {
        status = run_interactive(&election);
    } else if (opts.input == NULL || strcmp(opts.input, "-") == 0) {
        status = vote_read_stream(&election, stdin, opts.format, &err_line);
    } else {
        status = vote_read_file(&election, opts.input, opts.format, &err_line);
    }

    if (status != VOTE_OK) {
        if (err_line > 0) {
            fprintf(stderr, "%s: %s: line %lu: %s\n", prog,
                    (opts.input != NULL) ? opts.input : "<stdin>",
                    (unsigned long)err_line, vote_status_str(status));
        } else {
            fprintf(stderr, "%s: %s: %s\n", prog,
                    (opts.input != NULL) ? opts.input : "<stdin>",
                    vote_status_str(status));
        }
        vote_election_free(&election);
        return EXIT_FAILURE;
    }

    status = vote_election_run(&election, opts.system, &result);
    if (status != VOTE_OK) {
        fprintf(stderr, "%s: %s\n", prog, vote_status_str(status));
        vote_result_free(&result);
        vote_election_free(&election);
        return EXIT_FAILURE;
    }

    report.color   = want_color(opts.color_mode);
    report.quiet   = opts.quiet;
    report.summary = true;
    if (opts.interactive) {
        fputc('\n', stdout);
    }
    vote_report(stdout, &election, &result, &report);

    if (opts.export_path != NULL) {
        const char *dot = strrchr(opts.export_path, '.');
        bool        as_json = (dot != NULL &&
                               vote_ascii_casecmp(dot, ".json") == 0);

        status = as_json ? vote_export_json(opts.export_path, &election, &result)
                         : vote_export_text(opts.export_path, &election, &result);
        if (status != VOTE_OK) {
            fprintf(stderr, "%s: %s: %s\n", prog, opts.export_path,
                    vote_status_str(status));
            vote_result_free(&result);
            vote_election_free(&election);
            return EXIT_FAILURE;
        }
    }

    rc = result.decided ? EXIT_SUCCESS : EXIT_FAILURE;
    vote_result_free(&result);
    vote_election_free(&election);
    return rc;
}
