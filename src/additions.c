#include "additions.h"
#include "parser.h"

#include <string.h>

#define WILDCARD_ID ((int)ADDITION_COUNT)

static void skip_space(const char **s)
{
    while (**s == ' ' || **s == '\t') ++*s;
}

static bool take_token(const char **s, char *out, size_t capacity)
{
    size_t n = 0;
    while (token_char((unsigned char)**s)) {
        if (n + 1 >= capacity) return false;
        out[n++] = *(*s)++;
    }
    out[n] = '\0';
    return n != 0;
}

static bool parameter_value(const char **s, char *out, size_t capacity, bool *quoted)
{
    *quoted = **s == '"';
    if (!*quoted) return take_token(s, out, capacity);
    size_t n = 0;
    ++*s;
    while (**s && **s != '"') {
        unsigned char c = (unsigned char)*(*s)++;
        if (c == '\\') {
            if (!**s) return false;
            c = (unsigned char)*(*s)++;
        }
        if ((c < 32 && c != '\t') || c >= 127 || n + 1 >= capacity) return false;
        out[n++] = (char)c;
    }
    if (**s != '"') return false;
    ++*s;
    out[n] = '\0';
    return true;
}

static bool quality(const char *s, unsigned *q)
{
    if (*s != '0' && *s != '1') return false;
    unsigned result = (unsigned)(*s++ - '0') * 1000, scale = 100;
    if (*s == '.') {
        ++s;
        unsigned digits = 0;
        while (*s) {
            if (*s < '0' || *s > '9' || ++digits > 3) return false;
            if (result >= 1000 && *s != '0') return false;
            result += (unsigned)(*s++ - '0') * scale;
            scale /= 10;
        }
    } else if (*s) return false;
    *q = result;
    return true;
}

int additions_parse(const char *s, struct addition_preferences *preferences)
{
    memset(preferences, 0, sizeof(*preferences));
    for (;;) {
        skip_space(&s);
        if (*s == ',') { ++s; continue; }
        if (!*s) return 0;
        char name[64];
        struct addition_range entry = {-1, 1000, false};
        bool seen_q = false;
        if (preferences->count == MAX_ADDITION_ENTRIES || !take_token(&s, name, sizeof(name)))
            return 400;
        if (!strcmp(name, "*")) entry.id = WILDCARD_ID;
        for (unsigned i = 0; i < ADDITION_COUNT; ++i)
            if (ascii_equal(name, addition_names[i])) entry.id = (int)i;
        skip_space(&s);
        while (*s == ';') {
            char key[64], value[128];
            bool quoted = false, has_value = false;
            ++s;
            skip_space(&s);
            if (!take_token(&s, key, sizeof(key))) return 400;
            skip_space(&s);
            if (*s == '=') {
                ++s;
                skip_space(&s);
                if (!parameter_value(&s, value, sizeof(value), &quoted)) return 400;
                has_value = true;
            }
            if (ascii_equal(key, "q")) {
                if (seen_q || !has_value || quoted || !quality(value, &entry.quality)) return 400;
                seen_q = true;
            } else if (!seen_q) {
                if (!has_value) return 400;
                entry.preparation = true;
            }
            /* Accept extensions after q are syntactically checked, then ignored. */
            skip_space(&s);
        }
        preferences->ranges[preferences->count++] = entry;
        if (*s && *s != ',') return 400;
    }
}

/*
 * q values represent preference, not quantity.
 * Select the highest-quality supported addition within each category.
 * Additions from different categories may be combined.
 * q=0 marks an addition as unacceptable.
 */
int additions_select(const struct addition_preferences *preferences, uint32_t *mask)
{
    uint32_t forbidden = 0, explicit_names = 0;
    bool wildcard_forbidden = false, positive = false;
    unsigned wildcard_q = 0;
    int selected[ADD_CATEGORY_COUNT];
    unsigned best[ADD_CATEGORY_COUNT] = {0};
    for (unsigned c = 0; c < ADD_CATEGORY_COUNT; ++c) selected[c] = -1;
    *mask = 0;
    for (size_t i = 0; i < preferences->count; ++i) {
        const struct addition_range *entry = &preferences->ranges[i];
        if (entry->id >= 0 && entry->id < WILDCARD_ID) {
            uint32_t bit = UINT32_C(1) << (unsigned)entry->id;
            explicit_names |= bit;
            if (!entry->quality) forbidden |= bit;
        }
        if (entry->id == WILDCARD_ID) {
            if (!entry->quality) wildcard_forbidden = true;
            if (!entry->preparation && entry->quality > wildcard_q) wildcard_q = entry->quality;
        }
    }
    for (size_t i = 0; i < preferences->count; ++i) {
        const struct addition_range *entry = &preferences->ranges[i];
        if (!entry->quality) continue;
        if (entry->id == WILDCARD_ID) {
            if (!wildcard_forbidden) positive = true;
            continue;
        }
        if (entry->id >= 0 && (forbidden & (UINT32_C(1) << (unsigned)entry->id))) continue;
        positive = true;
        if (entry->id < 0 || entry->preparation) continue;
        unsigned category = (unsigned)addition_categories[entry->id];
        /* Strict comparison preserves wire order for equal-quality alternatives. */
        if (entry->quality > best[category]) {
            best[category] = entry->quality;
            selected[category] = entry->id;
        }
    }
    for (unsigned category = 0; category < ADD_CATEGORY_COUNT; ++category) {
        if (selected[category] < 0 && wildcard_q && !wildcard_forbidden) {
            for (unsigned i = 0; i < ADDITION_COUNT; ++i) {
                if (addition_categories[i] == (enum addition_category)category &&
                    !(explicit_names & (UINT32_C(1) << i))) {
                    selected[category] = (int)i;
                    break;
                }
            }
        }
        if (selected[category] >= 0) *mask |= UINT32_C(1) << (unsigned)selected[category];
    }
    return positive && !*mask ? 406 : 0;
}
