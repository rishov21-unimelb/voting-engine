/*
 * internal.h -- helpers shared between translation units but deliberately
 * kept out of the public API surface in include/voting.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VOTING_INTERNAL_H
#define VOTING_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

/* Locale-independent case-insensitive compare. strcasecmp is POSIX and
 * is not visible under -std=c11 -pedantic on every platform we target,
 * so the engine carries its own. */
int vote_ascii_casecmp(const char *a, const char *b);

/*
 * Geometric growth for the dynamic arrays behind CandidateSet, Ballot
 * and the parser scratch buffers.
 *
 * Returns the (possibly moved) allocation with room for at least
 * `needed` elements and updates *capacity, or NULL on overflow or
 * allocation failure -- in which case the original block is untouched
 * and still owned by the caller, who can free it without leaking.
 *
 * The block is returned rather than written back through a void**,
 * which would mean storing a void* into an object declared as some
 * other pointer type.
 */
void *vote_reserve(void *items, size_t *capacity, size_t needed,
                   size_t elem_size);

#endif /* VOTING_INTERNAL_H */
