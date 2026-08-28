/*
 * tally.c -- round-based counting, majority tests and tie-breaking.
 *
 * The counting rules implemented here:
 *
 *   instant-runoff  Each ballot contributes one vote to its highest
 *                   ranked candidate who is still standing. If nobody
 *                   holds an absolute majority of the votes that still
 *                   count, the lowest-scoring candidate is eliminated
 *                   and the next round redistributes their support.
 *                   Ballots whose preferences are all eliminated become
 *                   exhausted and leave the denominator, which is the
 *                   behaviour of a real distributive count.
 *
 *   borda           Positional scoring in a single round: on a ballot
 *                   over n candidates, the candidate ranked r collects
 *                   n - r points, so a first preference is worth n - 1.
 *
 *   plurality       First preferences only, single round, highest total
 *                   wins whether or not it is a majority.
 *
 * Ties are resolved deterministically -- see tally_lowest() -- so the
 * same input always produces the same transcript.
 *
 * SPDX-License-Identifier: MIT
 */
#include "voting.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Counting a single round                                             */
/* ------------------------------------------------------------------ */

vote_status_t vote_tally_round(const Election *e, VotingSystem system,
                               double *scores, double *out_total,
                               size_t *out_exhausted)
{
    size_t n, i, b;
    double total = 0.0;
    size_t exhausted = 0;

    if (e == NULL || scores == NULL) {
        return VOTE_ERR_INVALID;
    }
    n = e->candidates.count;
    if (n == 0) {
        return VOTE_ERR_EMPTY;
    }
    for (i = 0; i < n; ++i) {
        scores[i] = 0.0;
    }

    for (b = 0; b < e->ballots.count; ++b) {
        const Vote *v = &e->ballots.items[b];

        if (system == VOTE_SYSTEM_BORDA) {
            bool counted = false;
            for (i = 0; i < v->prefs.len && i < n; ++i) {
                unsigned r = v->prefs.ranks[i];
                if (r == 0u || e->candidates.items[i].eliminated) {
                    continue;
                }
                /* rank 1 is worth n-1 points, rank n is worth nothing */
                scores[i] += (double)(n - r);
                total     += (double)(n - r);
                counted = true;
            }
            if (!counted) {
                exhausted++;
            }
        } else {
            size_t top;
            if (vote_ballot_top_choice(e, v, &top)) {
                scores[top] += 1.0;
                total       += 1.0;
            } else {
                exhausted++;
            }
        }
    }

    if (out_total != NULL) {
        *out_total = total;
    }
    if (out_exhausted != NULL) {
        *out_exhausted = exhausted;
    }
    return VOTE_OK;
}

bool vote_tally_majority(const Election *e, const double *scores,
                         double total, size_t *out_index)
{
    size_t i;

    if (e == NULL || scores == NULL || total <= 0.0) {
        return false;
    }
    for (i = 0; i < e->candidates.count; ++i) {
        if (e->candidates.items[i].eliminated) {
            continue;
        }
        /* Strictly more than half: an exact 50/50 split is not a win. */
        if (scores[i] * 2.0 > total) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return true;
        }
    }
    return false;
}

/* Total first preferences a candidate received across every ballot,
 * ignoring eliminations. Used as the backward tie-break. */
static size_t first_preferences(const Election *e, size_t index)
{
    size_t b, count = 0;

    for (b = 0; b < e->ballots.count; ++b) {
        const Vote *v = &e->ballots.items[b];
        if (index < v->prefs.len && v->prefs.ranks[index] == 1u) {
            count++;
        }
    }
    return count;
}

bool vote_tally_lowest(const Election *e, const double *scores,
                       size_t *out_index, bool *out_tie)
{
    size_t i;
    size_t best = 0;
    size_t best_firsts = 0;
    bool   found = false;
    bool   tie = false;

    if (e == NULL || scores == NULL) {
        return false;
    }
    for (i = 0; i < e->candidates.count; ++i) {
        if (e->candidates.items[i].eliminated) {
            continue;
        }
        if (!found) {
            best        = i;
            best_firsts = first_preferences(e, i);
            found       = true;
            continue;
        }
        if (scores[i] < scores[best]) {
            best        = i;
            best_firsts = first_preferences(e, i);
        } else if (scores[i] == scores[best]) {
            /* Same score this round: prefer to cut whoever attracted
             * fewer first preferences overall, and fall back to
             * registration order so the outcome stays reproducible. */
            size_t firsts = first_preferences(e, i);
            tie = true;
            if (firsts < best_firsts) {
                best        = i;
                best_firsts = firsts;
            }
        }
    }
    if (!found) {
        return false;
    }
    if (out_index != NULL) {
        *out_index = best;
    }
    if (out_tie != NULL) {
        *out_tie = tie;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Assembling a round transcript                                       */
/* ------------------------------------------------------------------ */

static int entry_cmp(const void *lhs, const void *rhs)
{
    const TallyEntry *a = lhs;
    const TallyEntry *b = rhs;

    if (a->score > b->score) {
        return -1;
    }
    if (a->score < b->score) {
        return 1;
    }
    /* Registration order keeps qsort's instability from leaking into
     * the output when two candidates are level. */
    if (a->candidate < b->candidate) {
        return -1;
    }
    return (a->candidate > b->candidate) ? 1 : 0;
}

static vote_status_t stage_fill(ElectionStage *stage, const Election *e,
                                const double *scores, double total)
{
    size_t i, active = vote_candidate_active_count(e);
    size_t k = 0;

    stage->entries = NULL;
    stage->count   = 0;
    if (active == 0) {
        return VOTE_OK;
    }
    stage->entries = malloc(active * sizeof *stage->entries);
    if (stage->entries == NULL) {
        return VOTE_ERR_NOMEM;
    }
    for (i = 0; i < e->candidates.count; ++i) {
        if (e->candidates.items[i].eliminated) {
            continue;
        }
        stage->entries[k].candidate  = i;
        stage->entries[k].name       = e->candidates.items[i].name;
        stage->entries[k].score      = scores[i];
        stage->entries[k].percentage =
            (total > 0.0) ? (100.0 * scores[i] / total) : 0.0;
        k++;
    }
    stage->count = k;
    qsort(stage->entries, k, sizeof *stage->entries, entry_cmp);
    return VOTE_OK;
}

static vote_status_t result_push(ElectionResult *r, const ElectionStage *stage)
{
    ElectionStage *grown = vote_reserve(r->rounds, &r->capacity, r->count + 1u,
                                        sizeof *r->rounds);
    if (grown == NULL) {
        return VOTE_ERR_NOMEM;
    }
    r->rounds = grown;
    r->rounds[r->count] = *stage;
    r->count++;
    return VOTE_OK;
}

static void stage_init(ElectionStage *stage, size_t round)
{
    memset(stage, 0, sizeof *stage);
    stage->round = round;
}

/* ------------------------------------------------------------------ */
/* Driving a full election                                             */
/* ------------------------------------------------------------------ */

void vote_result_free(ElectionResult *result)
{
    size_t i;

    if (result == NULL) {
        return;
    }
    for (i = 0; i < result->count; ++i) {
        free(result->rounds[i].entries);
    }
    free(result->rounds);
    result->rounds   = NULL;
    result->count    = 0;
    result->capacity = 0;
    result->decided  = false;
    result->winner   = 0;
}

static void result_init(ElectionResult *r, VotingSystem system, size_t ballots)
{
    memset(r, 0, sizeof *r);
    r->system       = system;
    r->ballot_count = ballots;
}

vote_status_t vote_election_run(Election *e, VotingSystem system,
                                ElectionResult *result)
{
    double       *scores;
    size_t        n, round;
    vote_status_t status = VOTE_OK;

    if (e == NULL || result == NULL) {
        return VOTE_ERR_INVALID;
    }
    result_init(result, system, e->ballots.count);

    n = e->candidates.count;
    if (n == 0 || e->ballots.count == 0) {
        return VOTE_ERR_EMPTY;
    }
    scores = malloc(n * sizeof *scores);
    if (scores == NULL) {
        return VOTE_ERR_NOMEM;
    }
    vote_candidate_reset(e);

    /* Every round removes exactly one candidate, so n rounds is a hard
     * upper bound and the loop cannot spin. */
    for (round = 1; round <= n; ++round) {
        ElectionStage stage;
        double        total = 0.0;
        size_t        exhausted = 0;
        size_t        winner = 0, loser = 0;
        bool          tie = false;

        stage_init(&stage, round);
        status = vote_tally_round(e, system, scores, &total, &exhausted);
        if (status != VOTE_OK) {
            break;
        }
        stage.total     = total;
        stage.exhausted = exhausted;

        status = stage_fill(&stage, e, scores, total);
        if (status != VOTE_OK) {
            free(stage.entries);
            break;
        }

        if (system != VOTE_SYSTEM_INSTANT_RUNOFF) {
            /* Single-round systems: the top of the sorted table wins. */
            if (stage.count > 0 && total > 0.0) {
                stage.decided    = true;
                stage.winner     = stage.entries[0].candidate;
                stage.tie_broken = (stage.count > 1 &&
                                    stage.entries[0].score ==
                                        stage.entries[1].score);
            }
        } else if (vote_tally_majority(e, scores, total, &winner)) {
            stage.decided = true;
            stage.winner  = winner;
        } else if (vote_candidate_active_count(e) == 1u && stage.count == 1u &&
                   total > 0.0) {
            /* Last one standing takes the seat. */
            stage.decided = true;
            stage.winner  = stage.entries[0].candidate;
        } else if (total <= 0.0) {
            /* Every remaining ballot is exhausted: no winner is possible
             * and eliminating further candidates would be arbitrary. */
            stage.decided = false;
        } else if (vote_tally_lowest(e, scores, &loser, &tie)) {
            stage.eliminated_any = true;
            stage.eliminated     = loser;
            stage.tie_broken     = tie;
        }

        status = result_push(result, &stage);
        if (status != VOTE_OK) {
            free(stage.entries);
            break;
        }
        if (stage.decided) {
            result->decided = true;
            result->winner  = stage.winner;
            break;
        }
        if (!stage.eliminated_any) {
            break; /* nothing left to do and nobody elected */
        }
        status = vote_candidate_eliminate(e, stage.eliminated, round);
        if (status != VOTE_OK) {
            break;
        }
    }

    free(scores);
    return status;
}
