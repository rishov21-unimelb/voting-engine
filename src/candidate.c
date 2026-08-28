/*
 * candidate.c -- dynamic candidate registration, lookup and elimination.
 *
 * The registry replaces what used to be a fixed `char names[10][20]`
 * table with a geometrically growing array of owned strings, so the
 * engine scales to whatever the input file contains rather than to a
 * compile-time constant.
 *
 * SPDX-License-Identifier: MIT
 */
#include "voting.h"
#include "internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

const char *vote_status_str(vote_status_t status)
{
    switch (status) {
    case VOTE_OK:              return "ok";
    case VOTE_ERR_NOMEM:       return "out of memory";
    case VOTE_ERR_IO:          return "i/o error";
    case VOTE_ERR_PARSE:       return "malformed input";
    case VOTE_ERR_RANGE:       return "value out of range";
    case VOTE_ERR_INVALID:     return "invalid argument";
    case VOTE_ERR_DUPLICATE:   return "duplicate candidate";
    case VOTE_ERR_EMPTY:       return "election has no candidates or ballots";
    case VOTE_ERR_UNSUPPORTED: return "unsupported voting system or format";
    }
    return "unknown error";
}

char *vote_strdup(const char *s)
{
    size_t len;
    char  *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s) + 1u;
    copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

int vote_ascii_casecmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ------------------------------------------------------------------ */
/* System and format names                                             */
/* ------------------------------------------------------------------ */

vote_status_t vote_system_parse(const char *name, VotingSystem *out)
{
    if (name == NULL || out == NULL) {
        return VOTE_ERR_INVALID;
    }
    if (vote_ascii_casecmp(name, "instant-runoff") == 0 ||
        vote_ascii_casecmp(name, "instant_runoff") == 0 ||
        vote_ascii_casecmp(name, "irv") == 0 ||
        vote_ascii_casecmp(name, "preferential") == 0) {
        *out = VOTE_SYSTEM_INSTANT_RUNOFF;
        return VOTE_OK;
    }
    if (vote_ascii_casecmp(name, "borda") == 0 ||
        vote_ascii_casecmp(name, "borda-count") == 0) {
        *out = VOTE_SYSTEM_BORDA;
        return VOTE_OK;
    }
    if (vote_ascii_casecmp(name, "plurality") == 0 ||
        vote_ascii_casecmp(name, "fptp") == 0 ||
        vote_ascii_casecmp(name, "first-past-the-post") == 0) {
        *out = VOTE_SYSTEM_PLURALITY;
        return VOTE_OK;
    }
    return VOTE_ERR_UNSUPPORTED;
}

vote_status_t vote_format_parse(const char *name, VoteFormat *out)
{
    if (name == NULL || out == NULL) {
        return VOTE_ERR_INVALID;
    }
    if (vote_ascii_casecmp(name, "auto") == 0) {
        *out = VOTE_FORMAT_AUTO;
        return VOTE_OK;
    }
    if (vote_ascii_casecmp(name, "text") == 0 || vote_ascii_casecmp(name, "txt") == 0) {
        *out = VOTE_FORMAT_TEXT;
        return VOTE_OK;
    }
    if (vote_ascii_casecmp(name, "csv") == 0) {
        *out = VOTE_FORMAT_CSV;
        return VOTE_OK;
    }
    if (vote_ascii_casecmp(name, "json") == 0) {
        *out = VOTE_FORMAT_JSON;
        return VOTE_OK;
    }
    return VOTE_ERR_UNSUPPORTED;
}

const char *vote_system_name(VotingSystem system)
{
    switch (system) {
    case VOTE_SYSTEM_INSTANT_RUNOFF: return "instant-runoff";
    case VOTE_SYSTEM_BORDA:          return "borda";
    case VOTE_SYSTEM_PLURALITY:      return "plurality";
    }
    return "unknown";
}

const char *vote_format_name(VoteFormat format)
{
    switch (format) {
    case VOTE_FORMAT_AUTO: return "auto";
    case VOTE_FORMAT_TEXT: return "text";
    case VOTE_FORMAT_CSV:  return "csv";
    case VOTE_FORMAT_JSON: return "json";
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* Election lifecycle                                                  */
/* ------------------------------------------------------------------ */

void vote_election_init(Election *e)
{
    if (e == NULL) {
        return;
    }
    e->candidates.items    = NULL;
    e->candidates.count    = 0;
    e->candidates.capacity = 0;
    e->ballots.items       = NULL;
    e->ballots.count       = 0;
    e->ballots.capacity    = 0;
}

void vote_election_free(Election *e)
{
    size_t i;

    if (e == NULL) {
        return;
    }
    for (i = 0; i < e->candidates.count; ++i) {
        free(e->candidates.items[i].name);
    }
    free(e->candidates.items);

    for (i = 0; i < e->ballots.count; ++i) {
        free(e->ballots.items[i].prefs.ranks);
    }
    free(e->ballots.items);

    /* Re-initialising keeps a double free from turning into a crash. */
    vote_election_init(e);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

void *vote_reserve(void *items, size_t *capacity, size_t needed,
                   size_t elem_size)
{
    size_t cap = *capacity;
    void  *grown;

    if (items != NULL && needed <= cap) {
        return items;
    }
    if (cap == 0) {
        cap = 8u;
    }
    while (cap < needed) {
        if (cap > SIZE_MAX / 2u) {
            return NULL;
        }
        cap *= 2u;
    }
    if (cap > SIZE_MAX / elem_size) {
        return NULL;
    }
    grown = realloc(items, cap * elem_size);
    if (grown == NULL) {
        return NULL;
    }
    *capacity = cap;
    return grown;
}

vote_status_t vote_candidate_add(Election *e, const char *name,
                                 size_t *out_index)
{
    Candidate *slot;
    Candidate *grown;
    size_t     len;
    char      *copy;

    if (e == NULL || name == NULL) {
        return VOTE_ERR_INVALID;
    }
    len = strlen(name);
    if (len == 0) {
        return VOTE_ERR_PARSE;
    }
    if (len > VOTE_MAX_NAME_LEN) {
        return VOTE_ERR_RANGE;
    }
    if (e->candidates.count >= VOTE_MAX_CANDIDATES) {
        return VOTE_ERR_RANGE;
    }
    /* Ballots index candidates positionally, so late registration would
     * silently invalidate every ballot already stored. */
    if (e->ballots.count > 0) {
        return VOTE_ERR_INVALID;
    }
    if (vote_candidate_find(e, name) != NULL) {
        return VOTE_ERR_DUPLICATE;
    }
    grown = vote_reserve(e->candidates.items, &e->candidates.capacity,
                         e->candidates.count + 1u,
                         sizeof *e->candidates.items);
    if (grown == NULL) {
        return VOTE_ERR_NOMEM;
    }
    e->candidates.items = grown;

    copy = vote_strdup(name);
    if (copy == NULL) {
        return VOTE_ERR_NOMEM;
    }

    slot = &e->candidates.items[e->candidates.count];
    slot->name             = copy;
    slot->index            = e->candidates.count;
    slot->eliminated       = false;
    slot->eliminated_round = 0;

    if (out_index != NULL) {
        *out_index = e->candidates.count;
    }
    e->candidates.count++;
    return VOTE_OK;
}

const Candidate *vote_candidate_find(const Election *e, const char *name)
{
    size_t i;

    if (e == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < e->candidates.count; ++i) {
        if (vote_ascii_casecmp(e->candidates.items[i].name, name) == 0) {
            return &e->candidates.items[i];
        }
    }
    return NULL;
}

const Candidate *vote_candidate_at(const Election *e, size_t index)
{
    if (e == NULL || index >= e->candidates.count) {
        return NULL;
    }
    return &e->candidates.items[index];
}

size_t vote_candidate_count(const Election *e)
{
    return (e == NULL) ? 0u : e->candidates.count;
}

size_t vote_candidate_active_count(const Election *e)
{
    size_t i, active = 0;

    if (e == NULL) {
        return 0u;
    }
    for (i = 0; i < e->candidates.count; ++i) {
        if (!e->candidates.items[i].eliminated) {
            active++;
        }
    }
    return active;
}

/* ------------------------------------------------------------------ */
/* Elimination state                                                   */
/* ------------------------------------------------------------------ */

vote_status_t vote_candidate_eliminate(Election *e, size_t index, size_t round)
{
    if (e == NULL || index >= e->candidates.count) {
        return VOTE_ERR_INVALID;
    }
    if (e->candidates.items[index].eliminated) {
        return VOTE_ERR_INVALID;
    }
    e->candidates.items[index].eliminated       = true;
    e->candidates.items[index].eliminated_round = round;
    return VOTE_OK;
}

void vote_candidate_reset(Election *e)
{
    size_t i;

    if (e == NULL) {
        return;
    }
    for (i = 0; i < e->candidates.count; ++i) {
        e->candidates.items[i].eliminated       = false;
        e->candidates.items[i].eliminated_round = 0;
    }
}
