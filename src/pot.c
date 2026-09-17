#include "pot.h"

#include <assert.h>
#include <string.h>

const struct variety varieties[VARIETY_COUNT] = {
    {"darjeeling", 180000}, {"earl-grey", 240000}, {"peppermint", 300000}
};

const char *const addition_names[ADDITION_COUNT] = {
    "Cream", "Half-and-half", "Whole-milk", "Part-Skim", "Skim", "Non-Dairy",
    "Vanilla", "Almond", "Raspberry", "Chocolate", "Whisky", "Rum", "Kahlua",
    "Aquavit", "Sugar", "Xylitol", "Stevia"
};

const enum addition_category addition_categories[ADDITION_COUNT] = {
    ADD_MILK, ADD_MILK, ADD_MILK, ADD_MILK, ADD_MILK, ADD_MILK,
    ADD_SYRUP, ADD_SYRUP, ADD_SYRUP, ADD_SYRUP,
    ADD_ALCOHOL, ADD_ALCOHOL, ADD_ALCOHOL, ADD_ALCOHOL,
    ADD_SUGAR, ADD_SUGAR, ADD_SUGAR
};

void inventory_init(struct inventory *inventory, unsigned coffee, unsigned tea)
{
    assert(coffee + tea <= MAX_POTS);
    memset(inventory, 0, sizeof(*inventory));
    inventory->count = coffee + tea;
    for (size_t i = 0; i < inventory->count; ++i) {
        inventory->pots[i].kind = i < coffee ? POT_COFFEE : POT_TEA;
        inventory->pots[i].variety = -1;
    }
}

const char *pot_phase_name(enum pot_phase phase)
{
    switch (phase) {
    case POT_BREWING: return "brewing";
    case POT_ADDING: return "adding";
    case POT_READY: return "ready";
    default: return "idle";
    }
}

uint64_t pot_duration(const struct pot *pot)
{
    return pot->variety >= 0 ? varieties[pot->variety].brew_ms : 60000;
}

uint64_t pot_elapsed(const struct pot *pot, uint64_t now)
{
    uint64_t elapsed = pot->phase == POT_BREWING ?
        (now >= pot->started_ms ? now - pot->started_ms : 0) : pot->elapsed_ms;
    return elapsed > pot_duration(pot) ? pot_duration(pot) : elapsed;
}

static void finish(struct pot *pot, uint64_t now)
{
    pot->elapsed_ms = pot_elapsed(pot, now);
    pot->phase = pot->additions & MILK_MASK ? POT_ADDING : POT_READY;
}

bool pot_tick(struct pot *pot, uint64_t now)
{
    if (pot->phase != POT_BREWING || pot_elapsed(pot, now) < pot_duration(pot))
        return false;
    finish(pot, now);
    return true;
}

int pot_start(struct pot *pot, int variety, uint32_t additions, uint64_t now)
{
    if ((pot->kind == POT_COFFEE && variety != -1) ||
        (pot->kind == POT_TEA && (variety < 0 || variety >= (int)VARIETY_COUNT)))
        return 415;
    if (additions >> ADDITION_COUNT) return 406;
    if (pot->phase == POT_BREWING || pot->phase == POT_ADDING) return 409;
    pot->phase = POT_BREWING;
    pot->variety = variety;
    pot->additions = additions;
    pot->started_ms = now;
    pot->elapsed_ms = 0;
    if (pot->batches < UINT64_MAX) ++pot->batches;
    return 200;
}

int pot_stop(struct pot *pot, int variety, uint64_t now)
{
    if (pot->phase == POT_IDLE || pot->variety != variety) return 409;
    if (pot->phase == POT_BREWING) finish(pot, now);
    return 200;
}

int pot_when(struct pot *pot)
{
    if (pot->phase != POT_ADDING) return 409;
    pot->phase = POT_READY;
    return 200;
}
