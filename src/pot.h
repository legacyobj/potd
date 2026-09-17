#ifndef HTCPCP_POT_H
#define HTCPCP_POT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_POTS 16U
#define VARIETY_COUNT 3U
#define ADDITION_COUNT 17U
#define MILK_MASK UINT32_C(63)

enum pot_kind { POT_COFFEE, POT_TEA };
enum pot_phase { POT_IDLE, POT_BREWING, POT_ADDING, POT_READY };
enum addition_category { ADD_MILK, ADD_SYRUP, ADD_ALCOHOL, ADD_SUGAR, ADD_CATEGORY_COUNT };

struct variety { const char *name; uint64_t brew_ms; };
extern const struct variety varieties[VARIETY_COUNT];
extern const char *const addition_names[ADDITION_COUNT];
extern const enum addition_category addition_categories[ADDITION_COUNT];

struct pot {
    enum pot_kind kind;
    enum pot_phase phase;
    int variety;
    uint32_t additions;
    uint64_t started_ms;
    uint64_t elapsed_ms;
    uint64_t batches;
};

struct inventory { struct pot pots[MAX_POTS]; size_t count; };

void inventory_init(struct inventory *inventory, unsigned coffee, unsigned tea);
const char *pot_phase_name(enum pot_phase phase);
uint64_t pot_duration(const struct pot *pot);
uint64_t pot_elapsed(const struct pot *pot, uint64_t now);
bool pot_tick(struct pot *pot, uint64_t now);
int pot_start(struct pot *pot, int variety, uint32_t additions, uint64_t now);
int pot_stop(struct pot *pot, int variety, uint64_t now);
int pot_when(struct pot *pot);

#endif
