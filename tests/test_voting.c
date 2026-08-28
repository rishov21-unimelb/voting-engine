/*
 * test_voting.c -- unit tests for the voting engine.
 *
 * A deliberately tiny harness: no framework, no dependencies, just a
 * counter and a handful of macros. Each CHECK records a result and
 * keeps going, so one failure does not hide the rest of the suite.
 *
 * Build and run with `make test`.
 *
 * SPDX-License-Identifier: MIT
 */
#include "voting.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks_run = 0;
static unsigned checks_failed = 0;
static const char *current_case = "";

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks_run++;                                                      \
        if (!(cond)) {                                                     \
            checks_failed++;                                               \
            printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__,         \
                   current_case, #cond);                                   \
        }                                                                  \
    } while (0)

#define CHECK_EQ_SIZE(actual, expected)                                    \
    do {                                                                   \
        size_t a_ = (size_t)(actual);                                      \
        size_t e_ = (size_t)(expected);                                    \
        checks_run++;                                                      \
        if (a_ != e_) {                                                    \
            checks_failed++;                                               \
            printf("  FAIL %s:%d in %s: %s == %lu, expected %lu\n",        \
                   __FILE__, __LINE__, current_case, #actual,              \
                   (unsigned long)a_, (unsigned long)e_);                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_DBL(actual, expected)                                     \
    do {                                                                   \
        double a_ = (double)(actual);                                      \
        double e_ = (double)(expected);                                    \
        checks_run++;                                                      \
        if (fabs(a_ - e_) > 1e-9) {                                        \
            checks_failed++;                                               \
            printf("  FAIL %s:%d in %s: %s == %f, expected %f\n",          \
                   __FILE__, __LINE__, current_case, #actual, a_, e_);     \
        }                                                                  \
    } while (0)

#define CHECK_STR(actual, expected)                                        \
    do {                                                                   \
        const char *a_ = (actual);                                         \
        const char *e_ = (expected);                                       \
        checks_run++;                                                      \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                           \
            checks_failed++;                                               \
            printf("  FAIL %s:%d in %s: %s == \"%s\", expected \"%s\"\n",  \
                   __FILE__, __LINE__, current_case, #actual,              \
                   (a_ != NULL) ? a_ : "(null)", e_);                      \
        }                                                                  \
    } while (0)

#define TEST_CASE(name)                                                    \
    do {                                                                   \
        current_case = (name);                                             \
        printf("- %s\n", name);                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

/* Register `n` candidates named by the strings in `names`. */
static void seed_candidates(Election *e, const char *const *names, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (vote_candidate_add(e, names[i], NULL) != VOTE_OK) {
            printf("  FATAL: could not register %s\n", names[i]);
            exit(EXIT_FAILURE);
        }
    }
}

/* Add `count` identical ballots described by `ranks`. */
static void add_ballots(Election *e, const unsigned *ranks, size_t len,
                        size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (vote_ballot_add(e, ranks, len) != VOTE_OK) {
            printf("  FATAL: ballot rejected\n");
            exit(EXIT_FAILURE);
        }
    }
}

/* Rank the ballot at `ballot` gave candidate `cand`, or SIZE_MAX when
 * that ballot was never stored -- so a parse failure shows up as a bad
 * value rather than a crash inside the test suite. */
static size_t rank_of(const Election *e, size_t ballot, size_t cand)
{
    const Vote *v = vote_ballot_at(e, ballot);
    if (v == NULL || cand >= v->prefs.len) {
        return (size_t)-1;
    }
    return v->prefs.ranks[cand];
}

/* Name of the winner, or NULL when the election was undecided. */
static const char *winner_name(const Election *e, const ElectionResult *r)
{
    const Candidate *c;
    if (!r->decided) {
        return NULL;
    }
    c = vote_candidate_at(e, r->winner);
    return (c != NULL) ? c->name : NULL;
}

/* Write `text` to a scratch file so the file readers can be exercised
 * end to end. Returns false if the file could not be created. */
static bool write_temp(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return false;
    }
    fputs(text, fp);
    return fclose(fp) == 0;
}

/* ------------------------------------------------------------------ */
/* Candidate registry                                                  */
/* ------------------------------------------------------------------ */

static void test_candidate_registry(void)
{
    Election e;
    size_t   i, index = 999;
    char     name[32];

    TEST_CASE("candidate registration, lookup and duplicate rejection");
    vote_election_init(&e);

    CHECK(vote_candidate_add(&e, "Alice", &index) == VOTE_OK);
    CHECK_EQ_SIZE(index, 0);
    CHECK(vote_candidate_add(&e, "Bob", &index) == VOTE_OK);
    CHECK_EQ_SIZE(index, 1);

    /* Lookup is case-insensitive, and duplicates are refused. */
    CHECK(vote_candidate_find(&e, "alice") != NULL);
    CHECK(vote_candidate_find(&e, "Nobody") == NULL);
    CHECK(vote_candidate_add(&e, "ALICE", NULL) == VOTE_ERR_DUPLICATE);
    CHECK(vote_candidate_add(&e, "", NULL) == VOTE_ERR_PARSE);
    CHECK(vote_candidate_add(&e, NULL, NULL) == VOTE_ERR_INVALID);
    CHECK_EQ_SIZE(vote_candidate_count(&e), 2);

    vote_election_free(&e);

    TEST_CASE("registry grows past any fixed-size limit");
    vote_election_init(&e);
    /* The original program topped out at ten candidates; this pushes an
     * order of magnitude past that to prove the array really resizes. */
    for (i = 0; i < 250; ++i) {
        sprintf(name, "candidate_%03lu", (unsigned long)i);
        CHECK(vote_candidate_add(&e, name, NULL) == VOTE_OK);
    }
    CHECK_EQ_SIZE(vote_candidate_count(&e), 250);
    CHECK(vote_candidate_at(&e, 249) != NULL);
    CHECK_STR(vote_candidate_at(&e, 249)->name, "candidate_249");
    CHECK(vote_candidate_at(&e, 250) == NULL);
    vote_election_free(&e);

    TEST_CASE("free is idempotent");
    vote_election_free(&e); /* a second free must not crash or leak */
    CHECK_EQ_SIZE(vote_candidate_count(&e), 0);
}

/* ------------------------------------------------------------------ */
/* Ballot validation                                                   */
/* ------------------------------------------------------------------ */

static void test_ballot_validation(void)
{
    static const char *const names[] = { "Alice", "Bob", "Carol" };
    const unsigned good[]      = { 1u, 2u, 3u };
    const unsigned partial[]   = { 2u, 1u, 0u };
    const unsigned repeated[]  = { 1u, 1u, 2u };
    const unsigned gapped[]    = { 1u, 3u, 0u };
    const unsigned too_big[]   = { 1u, 2u, 9u };
    const unsigned all_blank[] = { 0u, 0u, 0u };
    Election e;

    TEST_CASE("preference orderings are validated before they are stored");
    CHECK(vote_preferences_validate(good, 3) == VOTE_OK);
    CHECK(vote_preferences_validate(partial, 3) == VOTE_OK);
    CHECK(vote_preferences_validate(repeated, 3) == VOTE_ERR_PARSE);
    CHECK(vote_preferences_validate(gapped, 3) == VOTE_ERR_PARSE);
    CHECK(vote_preferences_validate(too_big, 3) == VOTE_ERR_RANGE);
    CHECK(vote_preferences_validate(all_blank, 3) == VOTE_ERR_PARSE);
    CHECK(vote_preferences_validate(NULL, 3) == VOTE_ERR_INVALID);

    TEST_CASE("a rejected ballot leaves the election untouched");
    vote_election_init(&e);
    seed_candidates(&e, names, 3);
    CHECK(vote_ballot_add(&e, good, 3) == VOTE_OK);
    CHECK(vote_ballot_add(&e, repeated, 3) == VOTE_ERR_PARSE);
    CHECK(vote_ballot_add(&e, good, 2) == VOTE_ERR_INVALID);
    CHECK_EQ_SIZE(vote_ballot_count(&e), 1);
    CHECK(vote_ballot_at(&e, 0) != NULL && vote_ballot_at(&e, 0)->id == 1u);
    CHECK(vote_ballot_at(&e, 1) == NULL);

    /* Candidates may not appear after ballots: existing ballots index
     * candidates positionally and would silently mean something else. */
    CHECK(vote_candidate_add(&e, "Dave", NULL) == VOTE_ERR_INVALID);
    vote_election_free(&e);
}

/* ------------------------------------------------------------------ */
/* Majority and elimination                                            */
/* ------------------------------------------------------------------ */

static void test_majority(void)
{
    static const char *const names[] = { "Alice", "Bob", "Carol" };
    const unsigned a_first[] = { 1u, 2u, 3u };
    const unsigned b_first[] = { 2u, 1u, 3u };
    Election e;
    double   scores[3];
    double   total = 0.0;
    size_t   exhausted = 0, winner = 999;

    TEST_CASE("majority needs strictly more than half");
    vote_election_init(&e);
    seed_candidates(&e, names, 3);
    add_ballots(&e, a_first, 3, 5);
    add_ballots(&e, b_first, 3, 5);

    CHECK(vote_tally_round(&e, VOTE_SYSTEM_INSTANT_RUNOFF, scores, &total,
                           &exhausted) == VOTE_OK);
    CHECK_EQ_DBL(total, 10.0);
    CHECK_EQ_DBL(scores[0], 5.0);
    CHECK_EQ_DBL(scores[1], 5.0);
    CHECK_EQ_SIZE(exhausted, 0);
    /* Exactly half is not a majority. */
    CHECK(!vote_tally_majority(&e, scores, total, &winner));

    /* One more vote tips Alice over the line. */
    add_ballots(&e, a_first, 3, 1);
    CHECK(vote_tally_round(&e, VOTE_SYSTEM_INSTANT_RUNOFF, scores, &total,
                           &exhausted) == VOTE_OK);
    CHECK(vote_tally_majority(&e, scores, total, &winner));
    CHECK_EQ_SIZE(winner, 0);
    vote_election_free(&e);
}

static void test_elimination_and_redistribution(void)
{
    static const char *const names[] = { "Alice", "Bob", "Carol" };
    /* 4 x Alice, 3 x Bob, 2 x Carol-then-Bob.  Nobody has 5 of 9 in
     * round one; Carol is cut and her two ballots make Bob the winner
     * with 5, which is the behaviour a first-preference count misses. */
    const unsigned alice[] = { 1u, 2u, 3u };
    const unsigned bob[]   = { 3u, 1u, 2u };
    const unsigned carol[] = { 3u, 2u, 1u };
    Election       e;
    ElectionResult r;

    TEST_CASE("eliminating the trailing candidate redistributes their votes");
    vote_election_init(&e);
    seed_candidates(&e, names, 3);
    add_ballots(&e, alice, 3, 4);
    add_ballots(&e, bob, 3, 3);
    add_ballots(&e, carol, 3, 2);

    CHECK(vote_election_run(&e, VOTE_SYSTEM_INSTANT_RUNOFF, &r) == VOTE_OK);
    CHECK_EQ_SIZE(r.count, 2);
    CHECK_EQ_DBL(r.rounds[0].total, 9.0);
    CHECK(r.rounds[0].eliminated_any);
    CHECK_EQ_SIZE(r.rounds[0].eliminated, 2); /* Carol */
    CHECK(!r.rounds[0].decided);
    /* Round one leaves Alice ahead ... */
    CHECK_STR(r.rounds[0].entries[0].name, "Alice");
    CHECK_EQ_DBL(r.rounds[0].entries[0].score, 4.0);
    /* ... but the redistribution elects Bob. */
    CHECK(r.decided);
    CHECK_STR(winner_name(&e, &r), "Bob");
    CHECK_EQ_DBL(r.rounds[1].entries[0].score, 5.0);

    /* A plurality count on the same ballots gives the other answer. */
    vote_result_free(&r);
    CHECK(vote_election_run(&e, VOTE_SYSTEM_PLURALITY, &r) == VOTE_OK);
    CHECK_EQ_SIZE(r.count, 1);
    CHECK_STR(winner_name(&e, &r), "Alice");

    vote_result_free(&r);
    vote_election_free(&e);
}

static void test_exhausted_ballots(void)
{
    static const char *const names[] = { "Alice", "Bob", "Carol" };
    const unsigned alice_bob[] = { 1u, 2u, 0u };
    const unsigned bob_alice[] = { 2u, 1u, 0u };
    const unsigned carol_only[] = { 0u, 0u, 1u }; /* truncated: no fallback */
    Election       e;
    ElectionResult r;

    TEST_CASE("exhausted ballots leave the denominator");
    vote_election_init(&e);
    seed_candidates(&e, names, 3);
    add_ballots(&e, alice_bob, 3, 6);
    add_ballots(&e, bob_alice, 3, 5);
    add_ballots(&e, carol_only, 3, 4);

    CHECK(vote_election_run(&e, VOTE_SYSTEM_INSTANT_RUNOFF, &r) == VOTE_OK);
    CHECK_EQ_SIZE(r.count, 2);

    /* Round one: Alice 6, Bob 5, Carol 4 of 15. Nobody reaches 8, so
     * Carol goes and takes her four truncated ballots out of the count. */
    CHECK_EQ_DBL(r.rounds[0].total, 15.0);
    CHECK_EQ_SIZE(r.rounds[0].exhausted, 0);
    CHECK_EQ_SIZE(r.rounds[0].eliminated, 2);

    /* Round two counts 11 live ballots, and Alice's 6 clears half of
     * those. Against a fixed denominator of 15 she would still be two
     * short, so this is precisely where the two rules diverge. */
    CHECK_EQ_DBL(r.rounds[1].total, 11.0);
    CHECK_EQ_SIZE(r.rounds[1].exhausted, 4);
    CHECK(r.decided);
    CHECK_STR(winner_name(&e, &r), "Alice");
    CHECK_EQ_DBL(r.rounds[1].entries[0].score, 6.0);

    vote_result_free(&r);
    vote_election_free(&e);
}

/* ------------------------------------------------------------------ */
/* Tie-breaking                                                        */
/* ------------------------------------------------------------------ */

static void test_tie_breaking(void)
{
    static const char *const names[] = { "Alice", "Bob", "Carol", "Dave" };
    const unsigned alice[] = { 1u, 2u, 3u, 4u };
    const unsigned bob[]   = { 4u, 1u, 2u, 3u };
    const unsigned carol[] = { 4u, 3u, 1u, 2u };
    const unsigned dave[]  = { 4u, 3u, 2u, 1u };
    Election e;
    double   scores[4];
    size_t   loser = 999;
    bool     tie = false;

    TEST_CASE("tied candidates are cut in a deterministic order");
    vote_election_init(&e);
    seed_candidates(&e, names, 4);
    add_ballots(&e, alice, 4, 5);
    add_ballots(&e, bob, 4, 3);
    add_ballots(&e, carol, 4, 2);
    add_ballots(&e, dave, 4, 2);

    CHECK(vote_tally_round(&e, VOTE_SYSTEM_INSTANT_RUNOFF, scores, NULL,
                           NULL) == VOTE_OK);
    /* Carol and Dave are level on 2. Neither leads on first preferences
     * either, so registration order decides and Carol goes first. */
    CHECK(vote_tally_lowest(&e, scores, &loser, &tie));
    CHECK(tie);
    CHECK_EQ_SIZE(loser, 2);

    /* Cut her, and an eliminated candidate is never revisited. */
    CHECK(vote_candidate_eliminate(&e, loser, 1) == VOTE_OK);
    CHECK(vote_candidate_eliminate(&e, loser, 1) == VOTE_ERR_INVALID);
    CHECK_EQ_SIZE(vote_candidate_active_count(&e), 3);

    /* Carol's two ballots name Dave second, so the recount lifts him to
     * 4 and Bob is now the one at the bottom on 3. The next cut has to
     * follow the redistributed totals, not the round-one standings. */
    CHECK(vote_tally_round(&e, VOTE_SYSTEM_INSTANT_RUNOFF, scores, NULL,
                           NULL) == VOTE_OK);
    CHECK_EQ_DBL(scores[3], 4.0);
    CHECK_EQ_DBL(scores[1], 3.0);
    tie = false;
    CHECK(vote_tally_lowest(&e, scores, &loser, &tie));
    CHECK(!tie);
    CHECK_EQ_SIZE(loser, 1); /* Bob */

    vote_candidate_reset(&e);
    CHECK_EQ_SIZE(vote_candidate_active_count(&e), 4);
    vote_election_free(&e);
}

static void test_tie_break_prefers_fewer_first_preferences(void)
{
    static const char *const names[] = { "Alice", "Bob", "Carol" };
    /* Bob and Carol are level in this round, but Bob has attracted no
     * first preferences at all, so he is the one to go. */
    const unsigned alice[] = { 1u, 2u, 3u };
    const unsigned carol[] = { 3u, 2u, 1u };
    const unsigned bob_via_carol[] = { 3u, 1u, 2u };
    Election e;
    double   scores[3];
    size_t   loser = 999;
    bool     tie = false;

    TEST_CASE("a tie is broken on first preferences before ordering");
    vote_election_init(&e);
    seed_candidates(&e, names, 3);
    add_ballots(&e, alice, 3, 4);
    add_ballots(&e, carol, 3, 2);
    add_ballots(&e, bob_via_carol, 3, 2);

    CHECK(vote_tally_round(&e, VOTE_SYSTEM_INSTANT_RUNOFF, scores, NULL,
                           NULL) == VOTE_OK);
    CHECK_EQ_DBL(scores[1], 2.0);
    CHECK_EQ_DBL(scores[2], 2.0);
    CHECK(vote_tally_lowest(&e, scores, &loser, &tie));
    CHECK(tie);
    CHECK_EQ_SIZE(loser, 1); /* Bob: two votes here, but zero firsts */

    vote_election_free(&e);
}

/* ------------------------------------------------------------------ */
/* Borda                                                               */
/* ------------------------------------------------------------------ */

static void test_borda(void)
{
    static const char *const names[] = { "Alice", "Bob", "Carol" };
    const unsigned alice[] = { 1u, 2u, 3u };
    const unsigned bob[]   = { 3u, 1u, 2u };
    Election       e;
    ElectionResult r;

    TEST_CASE("borda scores positions rather than first preferences");
    vote_election_init(&e);
    seed_candidates(&e, names, 3);
    add_ballots(&e, alice, 3, 2); /* Alice 2x2=4, Bob 2x1=2, Carol 0 */
    add_ballots(&e, bob, 3, 2);   /* Bob 2x2=4, Carol 2x1=2, Alice 0 */

    CHECK(vote_election_run(&e, VOTE_SYSTEM_BORDA, &r) == VOTE_OK);
    CHECK_EQ_SIZE(r.count, 1);
    CHECK_EQ_DBL(r.rounds[0].total, 12.0);
    /* Alice 4, Bob 6, Carol 2: Bob wins on the strength of his second
     * preferences even though the first-preference split is even. */
    CHECK_STR(r.rounds[0].entries[0].name, "Bob");
    CHECK_EQ_DBL(r.rounds[0].entries[0].score, 6.0);
    CHECK_STR(winner_name(&e, &r), "Bob");

    vote_result_free(&r);
    vote_election_free(&e);
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static void test_parsers(void)
{
    static const char text[] =
        "# a comment, then the candidate count\n"
        "3\n"
        "Alice\n"
        "Bob\n"
        "Carol\n"
        "\n"
        "1 2 3\n"
        "2 1 3\n"
        "3 2 1\n";
    static const char csv[] =
        "Alice,Bob,Carol\n"
        "1,2,3\n"
        "2,1,3\n"
        ",1,2\n"; /* a blank cell means "not ranked" */
    static const char json[] =
        "{\n"
        "  \"title\": \"ignored key\",\n"
        "  \"candidates\": [\"Alice\", \"Bob\", \"Carol\"],\n"
        "  \"ballots\": [[1, 2, 3], [2, 1, 3], [\"Carol\", \"Bob\"]]\n"
        "}\n";
    Election e;
    size_t   line = 0;

    TEST_CASE("text format");
    vote_election_init(&e);
    CHECK(write_temp("test_input.txt", text));
    CHECK(vote_read_file(&e, "test_input.txt", VOTE_FORMAT_AUTO, &line)
          == VOTE_OK);
    CHECK_EQ_SIZE(vote_candidate_count(&e), 3);
    CHECK_EQ_SIZE(vote_ballot_count(&e), 3);
    CHECK(vote_candidate_at(&e, 2) != NULL);
    CHECK_STR(vote_candidate_at(&e, 2)->name, "Carol");
    vote_election_free(&e);

    TEST_CASE("csv format, including blank cells");
    vote_election_init(&e);
    CHECK(write_temp("test_input.csv", csv));
    CHECK(vote_read_file(&e, "test_input.csv", VOTE_FORMAT_AUTO, &line)
          == VOTE_OK);
    CHECK_EQ_SIZE(vote_candidate_count(&e), 3);
    CHECK_EQ_SIZE(vote_ballot_count(&e), 3);
    CHECK_EQ_SIZE(rank_of(&e, 2, 0), 0);
    vote_election_free(&e);

    TEST_CASE("json format, positional and name-ordered ballots");
    vote_election_init(&e);
    CHECK(write_temp("test_input.json", json));
    CHECK(vote_read_file(&e, "test_input.json", VOTE_FORMAT_AUTO, &line)
          == VOTE_OK);
    CHECK_EQ_SIZE(vote_candidate_count(&e), 3);
    CHECK_EQ_SIZE(vote_ballot_count(&e), 3);
    /* ["Carol","Bob"] means Carol first, Bob second, Alice unranked. */
    CHECK_EQ_SIZE(rank_of(&e, 2, 0), 0);
    CHECK_EQ_SIZE(rank_of(&e, 2, 1), 2);
    CHECK_EQ_SIZE(rank_of(&e, 2, 2), 1);
    vote_election_free(&e);

    remove("test_input.txt");
    remove("test_input.csv");
    remove("test_input.json");
}

static void test_malformed_input(void)
{
    /* Each of these must be refused rather than trusted, truncated or
     * allowed to run off the end of a buffer. */
    static const char *const bad[] = {
        "3\nAlice\nBob\n",                    /* names run out         */
        "3\nAlice\nBob\nCarol\n1 2\n",        /* short ballot row      */
        "3\nAlice\nBob\nCarol\n1 2 3 4\n",    /* long ballot row       */
        "3\nAlice\nBob\nCarol\n1 1 2\n",      /* repeated preference   */
        "3\nAlice\nBob\nCarol\n1 3 0\n",      /* gap in the ordering   */
        "3\nAlice\nBob\nCarol\n1 2 99\n",     /* rank out of range     */
        "3\nAlice\nBob\nCarol\n0 0 0\n",      /* no preferences at all */
        "3\nAlice\nBob\nAlice\n1 2 3\n",      /* duplicate candidate   */
        "0\n",                                /* nonsensical count     */
        "999999999999999999999\nAlice\n",     /* count overflows       */
        "3\nAlice\nBob\nCarol\n1 2 x\n",      /* not a number          */
        "",                                   /* empty file            */
        "{\"candidates\": [\"A\", \"B\"]",     /* truncated json        */
        "{\"candidates\": [], \"ballots\": []}",       /* no candidates */
        "{\"candidates\": [\"A\",\"B\"], \"ballots\": [[1,2],[\"Z\"]]}",
        "{\"candidates\": [\"A\",\"B\"], \"ballots\": [[1,2,3]]}",
    };
    size_t i;

    TEST_CASE("malformed input is rejected without crashing");
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        Election e;
        size_t   line = 0;
        vote_status_t status;

        vote_election_init(&e);
        CHECK(write_temp("test_bad.txt", bad[i]));
        status = vote_read_file(&e, "test_bad.txt", VOTE_FORMAT_AUTO, &line);
        checks_run++;
        if (status == VOTE_OK) {
            checks_failed++;
            printf("  FAIL %s:%d in %s: case %lu was accepted\n", __FILE__,
                   __LINE__, current_case, (unsigned long)i);
        }
        vote_election_free(&e);
    }
    remove("test_bad.txt");

    TEST_CASE("a missing file is an i/o error, not a crash");
    {
        Election e;
        vote_election_init(&e);
        CHECK(vote_read_file(&e, "no_such_file_here.txt", VOTE_FORMAT_AUTO,
                             NULL) == VOTE_ERR_IO);
        vote_election_free(&e);
    }

    TEST_CASE("long names and wide ballots are handled");
    {
        Election e;
        char     name[VOTE_MAX_NAME_LEN + 8];
        unsigned ranks[4];
        size_t   i2;

        vote_election_init(&e);
        memset(name, 'x', sizeof name - 1u);
        name[sizeof name - 1u] = '\0';
        CHECK(vote_candidate_add(&e, name, NULL) == VOTE_ERR_RANGE);

        name[VOTE_MAX_NAME_LEN] = '\0';
        CHECK(vote_candidate_add(&e, name, NULL) == VOTE_OK);
        CHECK(vote_candidate_add(&e, "Bob", NULL) == VOTE_OK);
        CHECK(vote_candidate_add(&e, "Carol", NULL) == VOTE_OK);
        CHECK(vote_candidate_add(&e, "Dave", NULL) == VOTE_OK);
        for (i2 = 0; i2 < 4; ++i2) {
            ranks[i2] = (unsigned)(i2 + 1u);
        }
        CHECK(vote_ballot_add(&e, ranks, 4) == VOTE_OK);
        vote_election_free(&e);
    }
}

/* ------------------------------------------------------------------ */
/* Export and CLI plumbing                                             */
/* ------------------------------------------------------------------ */

static void test_export(void)
{
    static const char *const names[] = { "Al \"Ace\" Ice", "Bob" };
    const unsigned a[] = { 1u, 2u };
    Election       e;
    ElectionResult r;
    char           buf[4096];
    size_t         got;
    FILE          *fp;

    TEST_CASE("json export escapes names and round-trips the transcript");
    vote_election_init(&e);
    seed_candidates(&e, names, 2);
    add_ballots(&e, a, 2, 3);
    CHECK(vote_election_run(&e, VOTE_SYSTEM_INSTANT_RUNOFF, &r) == VOTE_OK);
    CHECK(vote_export_json("test_out.json", &e, &r) == VOTE_OK);

    fp = fopen("test_out.json", "rb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        got = fread(buf, 1u, sizeof buf - 1u, fp);
        buf[got] = '\0';
        fclose(fp);
        CHECK(strstr(buf, "\\\"Ace\\\"") != NULL);
        CHECK(strstr(buf, "\"decided\": true") != NULL);
        CHECK(strstr(buf, "\"system\": \"instant-runoff\"") != NULL);
    }
    CHECK(vote_export_text("test_out.txt", &e, &r) == VOTE_OK);
    CHECK(vote_export_json("no_such_dir/x.json", &e, &r) == VOTE_ERR_IO);

    remove("test_out.json");
    remove("test_out.txt");
    vote_result_free(&r);
    vote_election_free(&e);
}

static void test_option_parsing(void)
{
    VotingSystem system;
    VoteFormat   format;

    TEST_CASE("cli spellings map to the right engine settings");
    CHECK(vote_system_parse("instant-runoff", &system) == VOTE_OK);
    CHECK(system == VOTE_SYSTEM_INSTANT_RUNOFF);
    CHECK(vote_system_parse("IRV", &system) == VOTE_OK);
    CHECK(system == VOTE_SYSTEM_INSTANT_RUNOFF);
    CHECK(vote_system_parse("borda", &system) == VOTE_OK);
    CHECK(system == VOTE_SYSTEM_BORDA);
    CHECK(vote_system_parse("fptp", &system) == VOTE_OK);
    CHECK(system == VOTE_SYSTEM_PLURALITY);
    CHECK(vote_system_parse("condorcet", &system) == VOTE_ERR_UNSUPPORTED);
    CHECK(vote_system_parse(NULL, &system) == VOTE_ERR_INVALID);

    CHECK(vote_format_parse("json", &format) == VOTE_OK);
    CHECK(format == VOTE_FORMAT_JSON);
    CHECK(vote_format_parse("yaml", &format) == VOTE_ERR_UNSUPPORTED);

    CHECK_STR(vote_system_name(VOTE_SYSTEM_BORDA), "borda");
    CHECK_STR(vote_format_name(VOTE_FORMAT_CSV), "csv");
    CHECK_STR(vote_status_str(VOTE_OK), "ok");
    CHECK(vote_status_str(VOTE_ERR_NOMEM)[0] != '\0');
}

/* ------------------------------------------------------------------ */
/* Scale                                                               */
/* ------------------------------------------------------------------ */

static void test_scale(void)
{
    Election       e;
    ElectionResult r;
    unsigned       ranks[16];
    size_t         i, b;
    char           name[32];

    TEST_CASE("50,000 ballots over 16 candidates");
    vote_election_init(&e);
    for (i = 0; i < 16; ++i) {
        sprintf(name, "cand%02lu", (unsigned long)i);
        CHECK(vote_candidate_add(&e, name, NULL) == VOTE_OK);
    }
    /* Rotate the ordering so every candidate leads on some ballots and
     * the count has to run several elimination rounds. */
    for (b = 0; b < 50000u; ++b) {
        size_t offset = b % 16u;
        for (i = 0; i < 16; ++i) {
            ranks[(i + offset) % 16u] = (unsigned)(i + 1u);
        }
        if (vote_ballot_add(&e, ranks, 16) != VOTE_OK) {
            CHECK(0);
            break;
        }
    }
    CHECK_EQ_SIZE(vote_ballot_count(&e), 50000);
    CHECK(vote_election_run(&e, VOTE_SYSTEM_INSTANT_RUNOFF, &r) == VOTE_OK);
    CHECK(r.decided);
    CHECK(r.count > 1);
    vote_result_free(&r);
    vote_election_free(&e);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("voting-engine %s test suite\n\n", VOTING_VERSION_STRING);

    test_candidate_registry();
    test_ballot_validation();
    test_majority();
    test_elimination_and_redistribution();
    test_exhausted_ballots();
    test_tie_breaking();
    test_tie_break_prefers_fewer_first_preferences();
    test_borda();
    test_parsers();
    test_malformed_input();
    test_export();
    test_option_parsing();
    test_scale();

    printf("\n%u checks, %u failed\n", checks_run, checks_failed);
    if (checks_failed != 0) {
        printf("FAILED\n");
        return EXIT_FAILURE;
    }
    printf("OK\n");
    return EXIT_SUCCESS;
}
