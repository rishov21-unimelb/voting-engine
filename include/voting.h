/*
 * voting.h -- public API for the preferential voting engine.
 *
 * The library models an election as a set of dynamically registered
 * candidates plus a growable box of ranked ballots. An election is
 * evaluated by one of several voting systems (instant-runoff, Borda
 * count, first-past-the-post) and yields a round-by-round transcript
 * that callers can render or serialise.
 *
 * Ownership rules
 * ---------------
 *   * Election owns every candidate name and every ballot it holds.
 *   * ElectionResult owns its rounds; the `name` field of a TallyEntry
 *     is a borrowed pointer into the Election and must not be freed.
 *   * Every vote_*_init() call must be balanced by the matching
 *     vote_*_free() call, which is always safe to call twice.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VOTING_H
#define VOTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOTING_VERSION_MAJOR 1
#define VOTING_VERSION_MINOR 0
#define VOTING_VERSION_PATCH 0
#define VOTING_VERSION_STRING "1.0.0"

/* Hard ceilings that exist purely to stop hostile input from asking the
 * allocator for absurd amounts of memory. Nothing in the engine assumes
 * these values, so they can be raised without touching the algorithms. */
#define VOTE_MAX_CANDIDATES 4096u
#define VOTE_MAX_NAME_LEN   255u

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    VOTE_OK = 0,          /* operation succeeded                       */
    VOTE_ERR_NOMEM,       /* allocation failed                         */
    VOTE_ERR_IO,          /* file could not be opened or read          */
    VOTE_ERR_PARSE,       /* input was malformed                       */
    VOTE_ERR_RANGE,       /* a value fell outside its legal range      */
    VOTE_ERR_INVALID,     /* arguments were inconsistent or NULL       */
    VOTE_ERR_DUPLICATE,   /* candidate name registered twice           */
    VOTE_ERR_EMPTY,       /* no candidates or no ballots to work with  */
    VOTE_ERR_UNSUPPORTED  /* unknown voting system or file format      */
} vote_status_t;

/* Human-readable description of a status code. Never returns NULL. */
const char *vote_status_str(vote_status_t status);

/* ------------------------------------------------------------------ */
/* Voting systems and input formats                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    VOTE_SYSTEM_INSTANT_RUNOFF = 0, /* eliminate and redistribute (default) */
    VOTE_SYSTEM_BORDA,              /* positional scoring, single round     */
    VOTE_SYSTEM_PLURALITY           /* first preferences only, single round */
} VotingSystem;

typedef enum {
    VOTE_FORMAT_AUTO = 0, /* sniff from file extension, then content       */
    VOTE_FORMAT_TEXT,     /* classic "count / names / rank rows" layout    */
    VOTE_FORMAT_CSV,      /* header row of names, one ballot per row       */
    VOTE_FORMAT_JSON      /* {"candidates":[...],"ballots":[[...]]}        */
} VoteFormat;

/* Parse a CLI spelling such as "instant-runoff", "irv" or "borda".
 * Returns VOTE_OK and writes *out on success. */
vote_status_t vote_system_parse(const char *name, VotingSystem *out);
vote_status_t vote_format_parse(const char *name, VoteFormat *out);

/* Canonical name of a system/format; never returns NULL. */
const char *vote_system_name(VotingSystem system);
const char *vote_format_name(VoteFormat format);

/* ------------------------------------------------------------------ */
/* Candidates                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *name;             /* owned, NUL-terminated                   */
    size_t index;            /* stable registration order, 0-based      */
    bool   eliminated;       /* mutated by the tally engine             */
    size_t eliminated_round; /* 1-based round of exit, 0 while active   */
} Candidate;

/* Auto-resizing candidate registry. */
typedef struct {
    Candidate *items;
    size_t     count;
    size_t     capacity;
} CandidateSet;

/* ------------------------------------------------------------------ */
/* Ballots                                                             */
/* ------------------------------------------------------------------ */

/*
 * One voter's ordering. ranks[i] is the preference this voter assigned
 * to the candidate registered at index i, where 1 is the most preferred
 * and 0 means "left blank". Non-zero entries must be distinct and must
 * form the contiguous run 1..k -- see vote_preferences_validate().
 */
typedef struct {
    unsigned *ranks; /* owned, `len` entries                        */
    size_t    len;   /* number of candidates this ballot ranks over */
} Preferences;

typedef struct {
    Preferences prefs;
    size_t      id; /* 1-based voter number, in reading order */
} Vote;

/* Auto-resizing box of ballots. */
typedef struct {
    Vote  *items;
    size_t count;
    size_t capacity;
} Ballot;

/* ------------------------------------------------------------------ */
/* Election                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    CandidateSet candidates;
    Ballot       ballots;
} Election;

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t      candidate;  /* index into Election.candidates          */
    const char *name;       /* borrowed from the Election, do not free */
    double      score;      /* votes (IRV/plurality) or points (Borda) */
    double      percentage; /* share of the round total, 0 if total==0 */
} TallyEntry;

/*
 * A single counting round. Entries cover only the candidates that were
 * still standing when the round was counted, sorted by score descending
 * with registration order breaking ties.
 */
typedef struct {
    size_t      round;          /* 1-based round number                  */
    TallyEntry *entries;        /* owned, `count` entries                */
    size_t      count;
    double      total;          /* sum of all scores counted this round  */
    size_t      exhausted;      /* ballots with no surviving preference  */
    bool        decided;        /* a winner was declared in this round   */
    size_t      winner;         /* candidate index, valid iff decided    */
    bool        eliminated_any; /* a candidate was cut in this round     */
    size_t      eliminated;     /* candidate index, valid iff above      */
    bool        tie_broken;     /* the cut required a tie-break rule     */
} ElectionStage;

typedef struct {
    ElectionStage *rounds; /* owned, `count` entries */
    size_t         count;
    size_t         capacity;
    VotingSystem   system;
    bool           decided;
    size_t         winner;       /* candidate index, valid iff decided */
    size_t         ballot_count;
} ElectionResult;

/* ------------------------------------------------------------------ */
/* Election lifecycle -- src/candidate.c                               */
/* ------------------------------------------------------------------ */

void vote_election_init(Election *e);
void vote_election_free(Election *e);

/* Register a candidate. Names are compared case-insensitively and
 * duplicates are rejected with VOTE_ERR_DUPLICATE. On success the new
 * candidate's index is written to *out_index when it is non-NULL. */
vote_status_t vote_candidate_add(Election *e, const char *name,
                                 size_t *out_index);

/* Case-insensitive lookup. Returns NULL when the name is unknown. */
const Candidate *vote_candidate_find(const Election *e, const char *name);
const Candidate *vote_candidate_at(const Election *e, size_t index);

size_t vote_candidate_count(const Election *e);
size_t vote_candidate_active_count(const Election *e);

/* Elimination state is owned by the tally engine but exposed so callers
 * can drive rounds by hand or write focused tests. */
vote_status_t vote_candidate_eliminate(Election *e, size_t index,
                                       size_t round);
void vote_candidate_reset(Election *e);

/* ------------------------------------------------------------------ */
/* Ballots and parsing -- src/ballot.c                                 */
/* ------------------------------------------------------------------ */

/* Copy `len` ranks into a new ballot. The ballot is validated before it
 * is stored, so a rejected ballot leaves the election untouched. */
vote_status_t vote_ballot_add(Election *e, const unsigned *ranks, size_t len);

/* Check that `ranks` is a legal preference ordering over `len`
 * candidates: values no greater than len, no repeats, and the non-zero
 * values forming the contiguous run 1..k for some k >= 1. */
vote_status_t vote_preferences_validate(const unsigned *ranks, size_t len);

size_t vote_ballot_count(const Election *e);
const Vote *vote_ballot_at(const Election *e, size_t index);

/* Index of the highest-ranked candidate on `v` that has not been
 * eliminated. Returns true on success; false means the ballot is
 * exhausted. */
bool vote_ballot_top_choice(const Election *e, const Vote *v,
                            size_t *out_index);

/* Readers. Every reader appends to an already-initialised Election and
 * leaves it usable (though possibly partially filled) on failure.
 * `line` receives the 1-based line number of a parse error when it is
 * non-NULL and the return value is VOTE_ERR_PARSE. */
vote_status_t vote_read_stream(Election *e, FILE *fp, VoteFormat format,
                               size_t *line);
vote_status_t vote_read_file(Election *e, const char *path,
                             VoteFormat format, size_t *line);

/* ------------------------------------------------------------------ */
/* Tallying -- src/tally.c                                             */
/* ------------------------------------------------------------------ */

/* Run a complete election. `result` is initialised by this call and
 * must be released with vote_result_free() even on failure. */
vote_status_t vote_election_run(Election *e, VotingSystem system,
                                ElectionResult *result);

void vote_result_free(ElectionResult *result);

/* Single-round primitives, exposed for testing and for callers that want
 * to drive the count themselves. `scores` must have room for
 * vote_candidate_count(e) doubles. */
vote_status_t vote_tally_round(const Election *e, VotingSystem system,
                               double *scores, double *out_total,
                               size_t *out_exhausted);

/* Index of a candidate holding a strict majority of `total`, or false
 * when no one does. */
bool vote_tally_majority(const Election *e, const double *scores,
                         double total, size_t *out_index);

/* Lowest-scoring active candidate. Ties are broken by fewest first
 * preferences overall, then by earliest registration. `out_tie` reports
 * whether the raw scores were tied. */
bool vote_tally_lowest(const Election *e, const double *scores,
                       size_t *out_index, bool *out_tie);

/* ------------------------------------------------------------------ */
/* Rendering and export -- src/cli.c                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    bool color;   /* emit ANSI escapes                      */
    bool quiet;   /* suppress the round-by-round transcript  */
    bool summary; /* print the closing summary block         */
} ReportOptions;

void vote_report(FILE *out, const Election *e, const ElectionResult *r,
                 const ReportOptions *opt);
vote_status_t vote_export_json(const char *path, const Election *e,
                               const ElectionResult *r);
vote_status_t vote_export_text(const char *path, const Election *e,
                               const ElectionResult *r);

/* Entry point used by src/main.c; returns a process exit status. */
int vote_cli_main(int argc, char **argv);

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

/* strdup() without the POSIX feature-test dance. Returns NULL on OOM
 * or when `s` is NULL. */
char *vote_strdup(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* VOTING_H */
