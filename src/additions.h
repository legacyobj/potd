#ifndef HTCPCP_ADDITIONS_H
#define HTCPCP_ADDITIONS_H

#include "pot.h"

#define MAX_ADDITION_ENTRIES 32U

struct addition_range {
    int id;
    unsigned quality;
    bool preparation;
};

struct addition_preferences {
    struct addition_range ranges[MAX_ADDITION_ENTRIES];
    size_t count;
};

int additions_parse(const char *text, struct addition_preferences *preferences);
int additions_select(const struct addition_preferences *preferences, uint32_t *mask);

#endif
