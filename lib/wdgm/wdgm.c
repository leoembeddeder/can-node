/* SPDX-License-Identifier: MIT */
#include "wdgm/wdgm.h"

#include <stddef.h>

/*
 * What the module does is described in wdgm.h; the comments here name the requirement each
 * branch implements. What holds for an initialised wdgm_state_t between any two calls, and what
 * state_valid() checks before wdgm_main_function() writes anything:
 *
 *   - status is OK, FAILED, EXPIRED or STOPPED, and expired_cycles <= expired_tolerance;
 *     in EXPIRED, expired_cycles >= 1;
 *   - first_expired is WDGM_NO_ENTITY or an identifier below entity_count;
 *   - every entity below entity_count is OK, FAILED or EXPIRED, with cycle < reference_cycle
 *     and failed_cycles <= failed_tolerance; failed_cycles is 0 in OK and at least 1 in FAILED.
 *
 * The SWS has no rule for a damaged state; making it STOPPED is this module's (wdgm.h).
 */

/* The checkpoint is called from interrupt handlers and tasks that must not wait: a counter that
 * needs a lock would make it block. uint32_t is unsigned int on x86-64 and unsigned long on
 * arm-none-eabi, so both have to be lock-free ("always", 2). */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "wdgm_checkpoint needs lock-free 32-bit atomics");
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2, "wdgm_checkpoint needs lock-free 32-bit atomics");
_Static_assert(WDGM_MAX_ENTITIES < WDGM_NO_ENTITY, "WDGM_NO_ENTITY is no identifier");
_Static_assert(WDGM_MAX_ENTITIES <= UINT8_MAX, "identifiers and counts are uint8_t");

static bool entity_config_valid(const wdgm_entity_config_t *entity)
{
    return (entity->expected_alive >= 1U) && (entity->min_margin < entity->expected_alive) &&
           (entity->reference_cycle >= 1U);
}

static bool config_valid(const wdgm_config_t *config)
{
    bool valid = (config->entities != NULL) && (config->entity_count >= 1U) &&
                 (config->entity_count <= WDGM_MAX_ENTITIES);

    for (uint32_t i = 0U; valid && (i < config->entity_count); i++) {
        valid = entity_config_valid(&config->entities[i]);
    }

    return valid;
}

static bool local_status_valid(const wdgm_entity_state_t *entity,
                               const wdgm_entity_config_t *config)
{
    bool valid = (entity->cycle < config->reference_cycle) &&
                 (entity->failed_cycles <= config->failed_tolerance);

    switch (entity->status) {
    case WDGM_LOCAL_STATUS_OK:
        valid = valid && (entity->failed_cycles == 0U);
        break;
    case WDGM_LOCAL_STATUS_FAILED:
        valid = valid && (entity->failed_cycles >= 1U);
        break;
    case WDGM_LOCAL_STATUS_EXPIRED:
        break;
    case WDGM_LOCAL_STATUS_DEACTIVATED:
    default:
        valid = false;
        break;
    }

    return valid;
}

static bool state_valid(const wdgm_state_t *state, const wdgm_config_t *config)
{
    bool valid =
        (state->expired_cycles <= config->expired_tolerance) &&
        ((state->first_expired == WDGM_NO_ENTITY) || (state->first_expired < state->entity_count));

    switch (state->status) {
    case WDGM_GLOBAL_STATUS_OK:
    case WDGM_GLOBAL_STATUS_FAILED:
    case WDGM_GLOBAL_STATUS_STOPPED:
        break;
    case WDGM_GLOBAL_STATUS_EXPIRED:
        valid = valid && (state->expired_cycles >= 1U);
        break;
    case WDGM_GLOBAL_STATUS_DEACTIVATED:
    default:
        valid = false;
        break;
    }

    for (uint32_t i = 0U; valid && (i < state->entity_count); i++) {
        valid = local_status_valid(&state->entity[i], &config->entities[i]);
    }

    return valid;
}

/* Every member of the state to its value before wdgm_init(), or after a refused one. */
static void deactivate(wdgm_state_t *state)
{
    for (uint32_t i = 0U; i < WDGM_MAX_ENTITIES; i++) {
        wdgm_entity_state_t *entity = &state->entity[i];

        atomic_store_explicit(&entity->alive, 0U, memory_order_relaxed);
        entity->last_count = 0U;
        entity->examinations = 0U;
        entity->incorrect = 0U;
        entity->cycle = 0U;
        entity->failed_cycles = 0U;
        entity->status = WDGM_LOCAL_STATUS_DEACTIVATED;
    }

    state->cycles = 0U;
    state->entity_count = 0U;
    state->expired_cycles = 0U;
    state->first_expired = WDGM_NO_ENTITY;
    state->status = WDGM_GLOBAL_STATUS_DEACTIVATED;
}

/* SWS_WdgM_00074, SWS_WdgM_00115: within (expected - min margin; expected + max margin). */
static bool alive_correct(uint32_t count, const wdgm_entity_config_t *config)
{
    const uint32_t lowest = (uint32_t)config->expected_alive - (uint32_t)config->min_margin;
    const uint32_t highest = (uint32_t)config->expected_alive + (uint32_t)config->max_margin;

    return (count >= lowest) && (count <= highest);
}

/* One step of Figure 2 after an examination of the entity `id`. */
static void local_step(wdgm_state_t *state, uint8_t id, const wdgm_entity_config_t *config,
                       bool correct)
{
    wdgm_entity_state_t *entity = &state->entity[id];
    const wdgm_local_status_t before = entity->status;

    if (before == WDGM_LOCAL_STATUS_OK) {
        if (correct) {
            /* SWS_WdgM_00201: stay OK */
        } else if (config->failed_tolerance == 0U) {
            entity->status = WDGM_LOCAL_STATUS_EXPIRED; /* SWS_WdgM_00202 */
        } else {
            entity->status = WDGM_LOCAL_STATUS_FAILED; /* SWS_WdgM_00203 */
            entity->failed_cycles = 1U;
        }
    } else if (before == WDGM_LOCAL_STATUS_FAILED) {
        if (!correct) {
            if (entity->failed_cycles < config->failed_tolerance) {
                entity->failed_cycles++; /* SWS_WdgM_00204 */
            } else {
                entity->status = WDGM_LOCAL_STATUS_EXPIRED; /* SWS_WdgM_00206 */
            }
        } else if (entity->failed_cycles > 1U) {
            entity->failed_cycles--; /* SWS_WdgM_00300 */
        } else {
            entity->failed_cycles = 0U; /* SWS_WdgM_00205 */
            entity->status = WDGM_LOCAL_STATUS_OK;
        }
    } else {
        /* EXPIRED cannot be left (state_valid() let nothing else through) */
    }

    if ((entity->status == WDGM_LOCAL_STATUS_EXPIRED) && (before != WDGM_LOCAL_STATUS_EXPIRED) &&
        (state->first_expired == WDGM_NO_ENTITY)) {
        state->first_expired = id; /* SWS_WdgM_00346 */
    }
}

/* One step of Figure 3, after every entity's step of this cycle. */
static void global_step(wdgm_state_t *state, const wdgm_config_t *config)
{
    bool any_failed = false;
    bool any_expired = false;

    for (uint32_t i = 0U; i < state->entity_count; i++) {
        any_failed = any_failed || (state->entity[i].status == WDGM_LOCAL_STATUS_FAILED);
        any_expired = any_expired || (state->entity[i].status == WDGM_LOCAL_STATUS_EXPIRED);
    }

    if ((state->status == WDGM_GLOBAL_STATUS_OK) || (state->status == WDGM_GLOBAL_STATUS_FAILED)) {
        if (any_expired) {
            if (config->expired_tolerance == 0U) {
                state->status = WDGM_GLOBAL_STATUS_STOPPED; /* SWS_WdgM_00216, SWS_WdgM_00117 */
            } else {
                state->status = WDGM_GLOBAL_STATUS_EXPIRED; /* SWS_WdgM_00215, SWS_WdgM_00077 */
                state->expired_cycles = 1U;
            }
        } else if (any_failed) {
            state->status = WDGM_GLOBAL_STATUS_FAILED; /* SWS_WdgM_00076, SWS_WdgM_00217 */
        } else {
            state->status = WDGM_GLOBAL_STATUS_OK; /* SWS_WdgM_00078, SWS_WdgM_00218 */
        }
    } else if (state->status == WDGM_GLOBAL_STATUS_EXPIRED) {
        /* An entity that expired stays so: in EXPIRED "at least one EXPIRED" always holds. */
        if (state->expired_cycles < config->expired_tolerance) {
            state->expired_cycles++; /* SWS_WdgM_00219 */
        } else {
            state->status = WDGM_GLOBAL_STATUS_STOPPED; /* SWS_WdgM_00220 */
        }
    } else {
        /* STOPPED: SWS_WdgM_00221, stay */
    }
}

bool wdgm_init(wdgm_state_t *state, const wdgm_config_t *config)
{
    bool ok = false;

    if (state != NULL) {
        deactivate(state);

        if ((config != NULL) && config_valid(config)) {
            for (uint32_t i = 0U; i < config->entity_count; i++) {
                state->entity[i].status = WDGM_LOCAL_STATUS_OK; /* SWS_WdgM_00268 */
            }

            state->entity_count = config->entity_count;
            state->status = WDGM_GLOBAL_STATUS_OK; /* SWS_WdgM_00285 */
            ok = true;
        }
    }

    return ok;
}

bool wdgm_checkpoint(wdgm_state_t *state, uint8_t entity)
{
    bool ok = false;

    /* entity_count was written by wdgm_init() before any checkpoint (wdgm.h, CONCURRENCY). The
     * second bound keeps a damaged count from sending the increment outside the state. */
    if ((state != NULL) && (entity < state->entity_count) && (entity < WDGM_MAX_ENTITIES)) {
        /* SWS_WdgM_00321. The returned old value is not needed. */
        (void)atomic_fetch_add_explicit(&state->entity[entity].alive, 1U, memory_order_relaxed);
        ok = true;
    }

    return ok;
}

wdgm_global_status_t wdgm_main_function(wdgm_state_t *state, const wdgm_config_t *config)
{
    wdgm_global_status_t result = WDGM_GLOBAL_STATUS_DEACTIVATED;

    if ((state != NULL) && (state->status != WDGM_GLOBAL_STATUS_DEACTIVATED)) {
        if ((config == NULL) || !config_valid(config) ||
            (config->entity_count != state->entity_count) || !state_valid(state, config)) {
            state->status = WDGM_GLOBAL_STATUS_STOPPED;
        } else {
            state->cycles++;

            for (uint8_t i = 0U; i < state->entity_count; i++) {
                wdgm_entity_state_t *entity = &state->entity[i];
                const wdgm_entity_config_t *entity_config = &config->entities[i];

                entity->cycle++;

                /* SWS_WdgM_00098: examine only at the end of a reference cycle. */
                if (entity->cycle >= entity_config->reference_cycle) {
                    const uint32_t count =
                        atomic_exchange_explicit(&entity->alive, 0U, memory_order_relaxed);
                    const bool correct = alive_correct(count, entity_config);

                    entity->cycle = 0U;
                    entity->last_count = count;
                    entity->examinations++;

                    if (!correct) {
                        entity->incorrect++;
                    }

                    local_step(state, i, entity_config, correct);
                }
            }

            global_step(state, config);
        }

        result = state->status;
    }

    return result;
}

wdgm_global_status_t wdgm_global_status(const wdgm_state_t *state)
{
    return (state != NULL) ? state->status : WDGM_GLOBAL_STATUS_DEACTIVATED;
}

wdgm_local_status_t wdgm_local_status(const wdgm_state_t *state, uint8_t entity)
{
    wdgm_local_status_t status = WDGM_LOCAL_STATUS_DEACTIVATED;

    if ((state != NULL) && (entity < state->entity_count) && (entity < WDGM_MAX_ENTITIES)) {
        status = state->entity[entity].status;
    }

    return status;
}

bool wdgm_may_kick(const wdgm_state_t *state)
{
    bool may_kick = false;

    if (state != NULL) {
        /* SWS_WdgM_00223, SWS_WdgM_00122 */
        may_kick = (state->status == WDGM_GLOBAL_STATUS_OK) ||
                   (state->status == WDGM_GLOBAL_STATUS_FAILED) ||
                   (state->status == WDGM_GLOBAL_STATUS_EXPIRED);
    }

    return may_kick;
}

uint8_t wdgm_first_expired(const wdgm_state_t *state)
{
    uint8_t first = WDGM_NO_ENTITY;

    /* Zeroed memory would otherwise name entity 0. */
    if ((state != NULL) && (state->status != WDGM_GLOBAL_STATUS_DEACTIVATED)) {
        first = state->first_expired;
    }

    return first;
}
