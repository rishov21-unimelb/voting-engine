/*
 * ballot.c -- preference storage, validation and input parsing.
 *
 * A ballot is stored positionally: ranks[i] is the preference the voter
 * gave to the candidate registered at index i. That layout keeps the
 * hot loop in the tally engine a flat scan over contiguous memory and
 * makes redistribution after an elimination a no-op -- nothing is
 * rewritten, the count simply skips eliminated indices.
 *
 * Three input formats are supported. All of them are parsed from a
 * fully buffered copy of the stream so that format sniffing never has
 * to push characters back, and so a malformed file can be rejected
 * without leaving the caller's Election half-built in a surprising way.
 *
 * SPDX-License-Identifier: MIT
 */
#include "voting.h"
#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VOTE_READ_CHUNK 4096u

/* ------------------------------------------------------------------ */
/* Preference validation and storage                                   */
/* ------------------------------------------------------------------ */

vote_status_t vote_preferences_validate(const unsigned *ranks, size_t len)
{
    unsigned char *seen;
    size_t         i, ranked = 0;

    if (ranks == NULL || len == 0) {
        return VOTE_ERR_INVALID;
    }
    seen = calloc(len + 1u, sizeof *seen);
    if (seen == NULL) {
        return VOTE_ERR_NOMEM;
    }
    for (i = 0; i < len; ++i) {
        unsigned r = ranks[i];
        if (r == 0u) {
            continue; /* deliberately left blank */
        }
        if (r > len) {
            free(seen);
            return VOTE_ERR_RANGE;
        }
        if (seen[r] != 0u) {
            free(seen);
            return VOTE_ERR_PARSE; /* the same preference used twice */
        }
        seen[r] = 1u;
        ranked++;
    }
    if (ranked == 0) {
        free(seen);
        return VOTE_ERR_PARSE; /* a ballot with no preferences at all */
    }
    /* The used preferences must be 1..ranked with no gaps, otherwise the
     * ordering is ambiguous once a candidate is eliminated. */
    for (i = 1; i <= ranked; ++i) {
        if (seen[i] == 0u) {
            free(seen);
            return VOTE_ERR_PARSE;
        }
    }
    free(seen);
    return VOTE_OK;
}

vote_status_t vote_ballot_add(Election *e, const unsigned *ranks, size_t len)
{
    vote_status_t status;
    unsigned     *copy;
    Vote         *slot;
    Vote         *grown;

    if (e == NULL || ranks == NULL) {
        return VOTE_ERR_INVALID;
    }
    if (e->candidates.count == 0) {
        return VOTE_ERR_EMPTY;
    }
    if (len != e->candidates.count) {
        return VOTE_ERR_INVALID;
    }
    status = vote_preferences_validate(ranks, len);
    if (status != VOTE_OK) {
        return status;
    }
    grown = vote_reserve(e->ballots.items, &e->ballots.capacity,
                         e->ballots.count + 1u, sizeof *e->ballots.items);
    if (grown == NULL) {
        return VOTE_ERR_NOMEM;
    }
    e->ballots.items = grown;

    copy = malloc(len * sizeof *copy);
    if (copy == NULL) {
        return VOTE_ERR_NOMEM;
    }
    memcpy(copy, ranks, len * sizeof *copy);

    slot = &e->ballots.items[e->ballots.count];
    slot->prefs.ranks = copy;
    slot->prefs.len   = len;
    slot->id          = e->ballots.count + 1u;
    e->ballots.count++;
    return VOTE_OK;
}

size_t vote_ballot_count(const Election *e)
{
    return (e == NULL) ? 0u : e->ballots.count;
}

const Vote *vote_ballot_at(const Election *e, size_t index)
{
    if (e == NULL || index >= e->ballots.count) {
        return NULL;
    }
    return &e->ballots.items[index];
}

bool vote_ballot_top_choice(const Election *e, const Vote *v,
                            size_t *out_index)
{
    size_t best = 0;
    bool   found = false;
    size_t i;

    if (e == NULL || v == NULL) {
        return false;
    }
    /* One pass over the ballot, keeping the smallest surviving rank,
     * beats the textbook rank-by-rank rescan for wide ballots. */
    for (i = 0; i < v->prefs.len && i < e->candidates.count; ++i) {
        unsigned r = v->prefs.ranks[i];
        if (r == 0u || e->candidates.items[i].eliminated) {
            continue;
        }
        if (!found || r < v->prefs.ranks[best]) {
            best  = i;
            found = true;
        }
    }
    if (found && out_index != NULL) {
        *out_index = best;
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* Buffered reading                                                    */
/* ------------------------------------------------------------------ */

/* Slurp a stream into a NUL-terminated heap buffer. */
static vote_status_t read_all(FILE *fp, char **out, size_t *out_len)
{
    char  *buf = NULL;
    size_t cap = 0, len = 0;

    for (;;) {
        size_t got;
        char  *grown = vote_reserve(buf, &cap, len + VOTE_READ_CHUNK + 1u,
                                    sizeof *buf);
        if (grown == NULL) {
            free(buf);
            return VOTE_ERR_NOMEM;
        }
        buf = grown;
        got = fread(buf + len, 1u, VOTE_READ_CHUNK, fp);
        len += got;
        if (got < VOTE_READ_CHUNK) {
            if (ferror(fp)) {
                free(buf);
                return VOTE_ERR_IO;
            }
            break;
        }
    }
    if (buf == NULL) {
        buf = calloc(1u, 1u);
        if (buf == NULL) {
            return VOTE_ERR_NOMEM;
        }
    }
    buf[len] = '\0';
    *out     = buf;
    *out_len = len;
    return VOTE_OK;
}

/* A cursor over the buffered input, carrying the line number so parse
 * failures can point the user at the offending row. */
typedef struct {
    const char *p;
    const char *end;
    size_t      line;
} Scanner;

static void scanner_init(Scanner *s, const char *buf, size_t len)
{
    s->p    = buf;
    s->end  = buf + len;
    s->line = 1u;
}

/* Hand back the next line (newline stripped) as a pointer/length pair
 * into the buffer. Returns false at end of input. */
static bool next_line(Scanner *s, const char **out, size_t *out_len)
{
    const char *start = s->p;
    const char *q     = s->p;

    if (s->p >= s->end) {
        return false;
    }
    while (q < s->end && *q != '\n') {
        ++q;
    }
    *out     = start;
    *out_len = (size_t)(q - start);
    /* Tolerate CRLF so files authored on Windows parse identically. */
    if (*out_len > 0 && start[*out_len - 1u] == '\r') {
        (*out_len)--;
    }
    s->p = (q < s->end) ? q + 1u : s->end;
    s->line++;
    return true;
}

static bool is_blank(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        if (isspace((unsigned char)s[i]) == 0) {
            return s[i] == '#'; /* '#' starts a comment line */
        }
    }
    return true;
}

/* Trim ASCII whitespace and matched double quotes from a field. */
static void trim_field(const char **s, size_t *len)
{
    const char *p = *s;
    size_t      n = *len;

    while (n > 0 && isspace((unsigned char)*p) != 0) {
        ++p;
        --n;
    }
    while (n > 0 && isspace((unsigned char)p[n - 1u]) != 0) {
        --n;
    }
    if (n >= 2u && *p == '"' && p[n - 1u] == '"') {
        ++p;
        n -= 2u;
    }
    *s   = p;
    *len = n;
}

/* Copy a bounded slice into a NUL-terminated buffer the caller owns. */
static char *slice_dup(const char *s, size_t len)
{
    char *copy = malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

/* Strict unsigned parse: the whole slice must be digits. */
static bool parse_uint(const char *s, size_t len, unsigned *out)
{
    unsigned long value = 0;
    size_t        i;

    trim_field(&s, &len);
    if (len == 0) {
        return false;
    }
    for (i = 0; i < len; ++i) {
        if (isdigit((unsigned char)s[i]) == 0) {
            return false;
        }
        if (value > (ULONG_MAX - 9u) / 10u) {
            return false; /* overflow before it can wrap */
        }
        value = value * 10u + (unsigned long)(s[i] - '0');
    }
    if (value > UINT_MAX) {
        return false;
    }
    *out = (unsigned)value;
    return true;
}

/* ------------------------------------------------------------------ */
/* Text format                                                         */
/* ------------------------------------------------------------------ */
/*
 *   3                <- candidate count
 *   Alice            <- one name per line (or several per line)
 *   Bob
 *   Carol
 *   1 2 3            <- one ballot per line, rank per candidate
 *   2 1 3
 *
 * Blank lines and lines starting with '#' are ignored.
 */

/* Split a line on whitespace or commas, yielding one field at a time. */
typedef struct {
    const char *p;
    const char *end;
    bool        commas;
} FieldIter;

static void fields_init(FieldIter *it, const char *line, size_t len,
                        bool commas)
{
    it->p      = line;
    it->end    = line + len;
    it->commas = commas;
}

static bool next_field(FieldIter *it, const char **out, size_t *out_len)
{
    const char *start;

    if (it->commas) {
        if (it->p > it->end) {
            return false;
        }
        start = it->p;
        while (it->p < it->end && *it->p != ',') {
            ++it->p;
        }
        *out     = start;
        *out_len = (size_t)(it->p - start);
        ++it->p; /* step past the comma (or past `end` to terminate) */
        return true;
    }
    while (it->p < it->end && isspace((unsigned char)*it->p) != 0) {
        ++it->p;
    }
    if (it->p >= it->end) {
        return false;
    }
    start = it->p;
    while (it->p < it->end && isspace((unsigned char)*it->p) == 0) {
        ++it->p;
    }
    *out     = start;
    *out_len = (size_t)(it->p - start);
    return true;
}

/* Read one row of ranks and append it as a ballot. */
static vote_status_t add_rank_row(Election *e, const char *line, size_t len,
                                  bool commas, unsigned *scratch)
{
    FieldIter   it;
    const char *field;
    size_t      field_len, seen = 0;
    size_t      n = e->candidates.count;

    fields_init(&it, line, len, commas);
    while (next_field(&it, &field, &field_len)) {
        if (seen >= n) {
            return VOTE_ERR_PARSE; /* more columns than candidates */
        }
        trim_field(&field, &field_len);
        if (field_len == 0 && commas) {
            scratch[seen++] = 0u; /* empty CSV cell means "unranked" */
            continue;
        }
        if (!parse_uint(field, field_len, &scratch[seen])) {
            return VOTE_ERR_PARSE;
        }
        seen++;
    }
    if (seen != n) {
        return VOTE_ERR_PARSE; /* short row */
    }
    return vote_ballot_add(e, scratch, n);
}

static vote_status_t parse_text(Election *e, const char *buf, size_t len,
                                size_t *err_line)
{
    Scanner       sc;
    const char   *line;
    size_t        line_len;
    unsigned      declared = 0;
    bool          have_count = false;
    unsigned     *scratch = NULL;
    vote_status_t status = VOTE_OK;

    scanner_init(&sc, buf, len);

    /* Phase 1: the declared candidate count. */
    while (!have_count && next_line(&sc, &line, &line_len)) {
        if (is_blank(line, line_len)) {
            continue;
        }
        if (!parse_uint(line, line_len, &declared) || declared == 0u ||
            declared > VOTE_MAX_CANDIDATES) {
            *err_line = sc.line - 1u;
            return VOTE_ERR_PARSE;
        }
        have_count = true;
    }
    if (!have_count) {
        return VOTE_ERR_EMPTY;
    }

    /* Phase 2: that many names, however they are spread across lines. */
    while (e->candidates.count < declared && next_line(&sc, &line, &line_len)) {
        FieldIter   it;
        const char *field;
        size_t      field_len;

        if (is_blank(line, line_len)) {
            continue;
        }
        fields_init(&it, line, line_len, false);
        while (e->candidates.count < declared &&
               next_field(&it, &field, &field_len)) {
            char *name = slice_dup(field, field_len);
            if (name == NULL) {
                return VOTE_ERR_NOMEM;
            }
            status = vote_candidate_add(e, name, NULL);
            free(name);
            if (status != VOTE_OK) {
                *err_line = sc.line - 1u;
                return status;
            }
        }
    }
    if (e->candidates.count != declared) {
        *err_line = sc.line;
        return VOTE_ERR_PARSE;
    }

    /* Phase 3: the ballots. */
    scratch = malloc(declared * sizeof *scratch);
    if (scratch == NULL) {
        return VOTE_ERR_NOMEM;
    }
    while (next_line(&sc, &line, &line_len)) {
        if (is_blank(line, line_len)) {
            continue;
        }
        status = add_rank_row(e, line, line_len, false, scratch);
        if (status != VOTE_OK) {
            *err_line = sc.line - 1u;
            free(scratch);
            return status;
        }
    }
    free(scratch);
    return VOTE_OK;
}

/* ------------------------------------------------------------------ */
/* CSV format                                                          */
/* ------------------------------------------------------------------ */
/*
 *   Alice,Bob,Carol
 *   1,2,3
 *   2,1,3
 */
static vote_status_t parse_csv(Election *e, const char *buf, size_t len,
                               size_t *err_line)
{
    Scanner       sc;
    const char   *line;
    size_t        line_len;
    bool          have_header = false;
    unsigned     *scratch = NULL;
    vote_status_t status = VOTE_OK;

    scanner_init(&sc, buf, len);

    while (!have_header && next_line(&sc, &line, &line_len)) {
        FieldIter   it;
        const char *field;
        size_t      field_len;

        if (is_blank(line, line_len)) {
            continue;
        }
        fields_init(&it, line, line_len, true);
        while (next_field(&it, &field, &field_len)) {
            char *name;
            trim_field(&field, &field_len);
            if (field_len == 0) {
                *err_line = sc.line - 1u;
                return VOTE_ERR_PARSE;
            }
            name = slice_dup(field, field_len);
            if (name == NULL) {
                return VOTE_ERR_NOMEM;
            }
            status = vote_candidate_add(e, name, NULL);
            free(name);
            if (status != VOTE_OK) {
                *err_line = sc.line - 1u;
                return status;
            }
        }
        have_header = true;
    }
    if (!have_header || e->candidates.count == 0) {
        return VOTE_ERR_EMPTY;
    }

    scratch = malloc(e->candidates.count * sizeof *scratch);
    if (scratch == NULL) {
        return VOTE_ERR_NOMEM;
    }
    while (next_line(&sc, &line, &line_len)) {
        if (is_blank(line, line_len)) {
            continue;
        }
        status = add_rank_row(e, line, line_len, true, scratch);
        if (status != VOTE_OK) {
            *err_line = sc.line - 1u;
            free(scratch);
            return status;
        }
    }
    free(scratch);
    return VOTE_OK;
}

/* ------------------------------------------------------------------ */
/* JSON format                                                         */
/* ------------------------------------------------------------------ */
/*
 * A deliberately small recursive-descent reader for the one schema the
 * engine cares about. Pulling in a third-party JSON library for this
 * would be the larger dependency, not the smaller one.
 *
 *   {
 *     "candidates": ["Alice", "Bob", "Carol"],
 *     "ballots": [[1, 2, 3], ["Carol", "Alice"]]
 *   }
 *
 * A ballot may be given either as positional ranks (numbers, aligned
 * with the candidate list) or as candidate names in preference order.
 */

static void json_skip_ws(Scanner *s)
{
    while (s->p < s->end) {
        char c = *s->p;
        if (c == '\n') {
            s->line++;
            ++s->p;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            ++s->p;
        } else {
            break;
        }
    }
}

static bool json_peek(Scanner *s, char *out)
{
    json_skip_ws(s);
    if (s->p >= s->end) {
        return false;
    }
    *out = *s->p;
    return true;
}

static bool json_eat(Scanner *s, char expect)
{
    char c;
    if (!json_peek(s, &c) || c != expect) {
        return false;
    }
    ++s->p;
    return true;
}

/* Encode one Unicode code point as UTF-8 into `dst`, returning the byte
 * count written (at most 4). */
static size_t utf8_encode(unsigned long cp, char *dst)
{
    if (cp < 0x80u) {
        dst[0] = (char)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    dst[0] = (char)(0xF0u | (cp >> 18));
    dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4u;
}

static bool json_hex4(Scanner *s, unsigned long *out)
{
    unsigned long value = 0;
    int           i;

    for (i = 0; i < 4; ++i) {
        int c;
        if (s->p >= s->end) {
            return false;
        }
        c = (unsigned char)*s->p++;
        if (isdigit(c)) {
            value = value * 16u + (unsigned long)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value = value * 16u + (unsigned long)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value = value * 16u + (unsigned long)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = value;
    return true;
}

/* Parse a JSON string into a freshly allocated C string. */
static bool json_string(Scanner *s, char **out)
{
    char  *buf = NULL;
    char  *grown;
    size_t cap = 0, len = 0;

    if (!json_eat(s, '"')) {
        return false;
    }
    for (;;) {
        char c;
        char encoded[4];
        size_t written = 0;

        if (s->p >= s->end) {
            free(buf);
            return false;
        }
        c = *s->p++;
        if (c == '"') {
            break;
        }
        if (c == '\\') {
            if (s->p >= s->end) {
                free(buf);
                return false;
            }
            switch (*s->p++) {
            case '"':  encoded[written++] = '"';  break;
            case '\\': encoded[written++] = '\\'; break;
            case '/':  encoded[written++] = '/';  break;
            case 'b':  encoded[written++] = '\b'; break;
            case 'f':  encoded[written++] = '\f'; break;
            case 'n':  encoded[written++] = '\n'; break;
            case 'r':  encoded[written++] = '\r'; break;
            case 't':  encoded[written++] = '\t'; break;
            case 'u': {
                unsigned long cp;
                if (!json_hex4(s, &cp)) {
                    free(buf);
                    return false;
                }
                /* Recombine a surrogate pair into one code point. */
                if (cp >= 0xD800u && cp <= 0xDBFFu &&
                    s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                    unsigned long low;
                    const char   *save = s->p;
                    s->p += 2;
                    if (json_hex4(s, &low) && low >= 0xDC00u && low <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) +
                             (low - 0xDC00u);
                    } else {
                        s->p = save;
                    }
                }
                written = utf8_encode(cp, encoded);
                break;
            }
            default:
                free(buf);
                return false;
            }
        } else if ((unsigned char)c < 0x20u) {
            free(buf); /* raw control characters are not legal JSON */
            return false;
        } else {
            if (c == '\n') {
                s->line++;
            }
            encoded[written++] = c;
        }

        grown = vote_reserve(buf, &cap, len + written + 1u, sizeof *buf);
        if (grown == NULL) {
            free(buf);
            return false;
        }
        buf = grown;
        memcpy(buf + len, encoded, written);
        len += written;
    }
    grown = vote_reserve(buf, &cap, len + 1u, sizeof *buf);
    if (grown == NULL) {
        free(buf);
        return false;
    }
    buf = grown;
    buf[len] = '\0';
    *out     = buf;
    return true;
}

static bool json_number(Scanner *s, double *out)
{
    char   *stop = NULL;
    double  value;

    json_skip_ws(s);
    errno = 0;
    value = strtod(s->p, &stop);
    if (stop == s->p || stop > s->end || errno == ERANGE) {
        return false;
    }
    s->p = stop;
    *out = value;
    return true;
}

static bool json_skip_value(Scanner *s);

static bool json_skip_container(Scanner *s, char open, char close)
{
    char c;

    if (!json_eat(s, open)) {
        return false;
    }
    if (json_peek(s, &c) && c == close) {
        ++s->p;
        return true;
    }
    for (;;) {
        if (open == '{') {
            char *key = NULL;
            if (!json_string(s, &key)) {
                return false;
            }
            free(key);
            if (!json_eat(s, ':')) {
                return false;
            }
        }
        if (!json_skip_value(s)) {
            return false;
        }
        if (!json_peek(s, &c)) {
            return false;
        }
        if (c == ',') {
            ++s->p;
            continue;
        }
        return json_eat(s, close);
    }
}

static bool json_skip_value(Scanner *s)
{
    char c;

    if (!json_peek(s, &c)) {
        return false;
    }
    if (c == '"') {
        char *tmp = NULL;
        if (!json_string(s, &tmp)) {
            return false;
        }
        free(tmp);
        return true;
    }
    if (c == '{') {
        return json_skip_container(s, '{', '}');
    }
    if (c == '[') {
        return json_skip_container(s, '[', ']');
    }
    if (c == 't' || c == 'f' || c == 'n') {
        while (s->p < s->end && isalpha((unsigned char)*s->p) != 0) {
            ++s->p;
        }
        return true;
    }
    {
        double ignored;
        return json_number(s, &ignored);
    }
}

static vote_status_t json_candidates(Scanner *s, Election *e)
{
    char c;

    if (!json_eat(s, '[')) {
        return VOTE_ERR_PARSE;
    }
    if (json_peek(s, &c) && c == ']') {
        ++s->p;
        return VOTE_ERR_EMPTY;
    }
    for (;;) {
        char         *name = NULL;
        vote_status_t status;

        if (!json_string(s, &name)) {
            return VOTE_ERR_PARSE;
        }
        status = vote_candidate_add(e, name, NULL);
        free(name);
        if (status != VOTE_OK) {
            return status;
        }
        if (!json_peek(s, &c)) {
            return VOTE_ERR_PARSE;
        }
        if (c == ',') {
            ++s->p;
            continue;
        }
        return json_eat(s, ']') ? VOTE_OK : VOTE_ERR_PARSE;
    }
}

/* One ballot: either [1,2,3] positional ranks or ["Bob","Alice"] names
 * listed most-preferred first. */
static vote_status_t json_one_ballot(Scanner *s, Election *e,
                                     unsigned *scratch)
{
    size_t n = e->candidates.count;
    size_t i, filled = 0;
    char   c;

    for (i = 0; i < n; ++i) {
        scratch[i] = 0u;
    }
    if (!json_eat(s, '[')) {
        return VOTE_ERR_PARSE;
    }
    if (json_peek(s, &c) && c == ']') {
        ++s->p;
        return VOTE_ERR_PARSE; /* an empty ballot ranks nobody */
    }
    for (;;) {
        if (!json_peek(s, &c)) {
            return VOTE_ERR_PARSE;
        }
        if (c == '"') {
            char            *name = NULL;
            const Candidate *cand;

            if (!json_string(s, &name)) {
                return VOTE_ERR_PARSE;
            }
            cand = vote_candidate_find(e, name);
            free(name);
            if (cand == NULL) {
                return VOTE_ERR_PARSE; /* names a candidate who is not standing */
            }
            if (scratch[cand->index] != 0u) {
                return VOTE_ERR_PARSE; /* the same candidate listed twice */
            }
            scratch[cand->index] = (unsigned)(filled + 1u);
            filled++;
        } else {
            double value;
            if (filled >= n) {
                return VOTE_ERR_PARSE;
            }
            if (!json_number(s, &value)) {
                return VOTE_ERR_PARSE;
            }
            if (value < 0.0 || value > (double)n ||
                value != (double)(long)value) {
                return VOTE_ERR_RANGE;
            }
            scratch[filled] = (unsigned)(long)value;
            filled++;
        }
        if (!json_peek(s, &c)) {
            return VOTE_ERR_PARSE;
        }
        if (c == ',') {
            ++s->p;
            continue;
        }
        if (!json_eat(s, ']')) {
            return VOTE_ERR_PARSE;
        }
        break;
    }
    return vote_ballot_add(e, scratch, n);
}

static vote_status_t json_ballots(Scanner *s, Election *e)
{
    unsigned     *scratch;
    vote_status_t status;
    char          c;

    if (e->candidates.count == 0) {
        return VOTE_ERR_PARSE; /* "candidates" must come first */
    }
    if (!json_eat(s, '[')) {
        return VOTE_ERR_PARSE;
    }
    if (json_peek(s, &c) && c == ']') {
        ++s->p;
        return VOTE_OK;
    }
    scratch = malloc(e->candidates.count * sizeof *scratch);
    if (scratch == NULL) {
        return VOTE_ERR_NOMEM;
    }
    for (;;) {
        status = json_one_ballot(s, e, scratch);
        if (status != VOTE_OK) {
            free(scratch);
            return status;
        }
        if (!json_peek(s, &c)) {
            free(scratch);
            return VOTE_ERR_PARSE;
        }
        if (c == ',') {
            ++s->p;
            continue;
        }
        free(scratch);
        return json_eat(s, ']') ? VOTE_OK : VOTE_ERR_PARSE;
    }
}

static vote_status_t parse_json(Election *e, const char *buf, size_t len,
                                size_t *err_line)
{
    Scanner       sc;
    vote_status_t status;
    char          c;

    scanner_init(&sc, buf, len);
    if (!json_eat(&sc, '{')) {
        *err_line = sc.line;
        return VOTE_ERR_PARSE;
    }
    if (json_peek(&sc, &c) && c == '}') {
        return VOTE_ERR_EMPTY;
    }
    for (;;) {
        char *key = NULL;

        if (!json_string(&sc, &key)) {
            *err_line = sc.line;
            return VOTE_ERR_PARSE;
        }
        if (!json_eat(&sc, ':')) {
            free(key);
            *err_line = sc.line;
            return VOTE_ERR_PARSE;
        }
        if (vote_ascii_casecmp(key, "candidates") == 0) {
            status = json_candidates(&sc, e);
        } else if (vote_ascii_casecmp(key, "ballots") == 0 ||
                   vote_ascii_casecmp(key, "votes") == 0) {
            status = json_ballots(&sc, e);
        } else {
            status = json_skip_value(&sc) ? VOTE_OK : VOTE_ERR_PARSE;
        }
        free(key);
        if (status != VOTE_OK) {
            *err_line = sc.line;
            return status;
        }
        if (!json_peek(&sc, &c)) {
            *err_line = sc.line;
            return VOTE_ERR_PARSE;
        }
        if (c == ',') {
            ++sc.p;
            continue;
        }
        if (!json_eat(&sc, '}')) {
            *err_line = sc.line;
            return VOTE_ERR_PARSE;
        }
        break;
    }
    if (e->candidates.count == 0) {
        return VOTE_ERR_EMPTY;
    }
    return VOTE_OK;
}

/* ------------------------------------------------------------------ */
/* Format sniffing and public readers                                  */
/* ------------------------------------------------------------------ */

/* Decide a format from the content alone: a leading '{' means JSON, and
 * a first meaningful line whose first field is not a bare integer means
 * the CSV header row. Everything else is the classic text layout. */
static VoteFormat sniff(const char *buf, size_t len)
{
    Scanner     sc;
    const char *line;
    size_t      line_len;

    scanner_init(&sc, buf, len);
    while (next_line(&sc, &line, &line_len)) {
        FieldIter   it;
        const char *field;
        size_t      field_len;
        unsigned    ignored;

        if (is_blank(line, line_len)) {
            continue;
        }
        trim_field(&line, &line_len);
        if (line_len > 0 && line[0] == '{') {
            return VOTE_FORMAT_JSON;
        }
        fields_init(&it, line, line_len, true);
        if (!next_field(&it, &field, &field_len)) {
            break;
        }
        trim_field(&field, &field_len);
        if (!parse_uint(field, field_len, &ignored)) {
            return VOTE_FORMAT_CSV;
        }
        /* A bare count on its own line is the text header; a numeric
         * first cell followed by more cells is a headerless CSV, which
         * we cannot name candidates from, so text it is. */
        return VOTE_FORMAT_TEXT;
    }
    return VOTE_FORMAT_TEXT;
}

vote_status_t vote_read_stream(Election *e, FILE *fp, VoteFormat format,
                               size_t *line)
{
    char         *buf = NULL;
    size_t        len = 0, err_line = 0;
    vote_status_t status;

    if (e == NULL || fp == NULL) {
        return VOTE_ERR_INVALID;
    }
    status = read_all(fp, &buf, &len);
    if (status != VOTE_OK) {
        return status;
    }
    if (format == VOTE_FORMAT_AUTO) {
        format = sniff(buf, len);
    }
    switch (format) {
    case VOTE_FORMAT_TEXT: status = parse_text(e, buf, len, &err_line); break;
    case VOTE_FORMAT_CSV:  status = parse_csv(e, buf, len, &err_line);  break;
    case VOTE_FORMAT_JSON: status = parse_json(e, buf, len, &err_line); break;
    default:               status = VOTE_ERR_UNSUPPORTED;              break;
    }
    free(buf);
    if (line != NULL) {
        *line = err_line;
    }
    if (status == VOTE_OK && e->ballots.count == 0) {
        return VOTE_ERR_EMPTY;
    }
    return status;
}

vote_status_t vote_read_file(Election *e, const char *path, VoteFormat format,
                             size_t *line)
{
    FILE         *fp;
    vote_status_t status;

    if (e == NULL || path == NULL) {
        return VOTE_ERR_INVALID;
    }
    /* An explicit extension beats content sniffing. */
    if (format == VOTE_FORMAT_AUTO) {
        const char *dot = strrchr(path, '.');
        if (dot != NULL) {
            if (vote_ascii_casecmp(dot, ".json") == 0) {
                format = VOTE_FORMAT_JSON;
            } else if (vote_ascii_casecmp(dot, ".csv") == 0) {
                format = VOTE_FORMAT_CSV;
            }
        }
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return VOTE_ERR_IO;
    }
    status = vote_read_stream(e, fp, format, line);
    fclose(fp);
    return status;
}
