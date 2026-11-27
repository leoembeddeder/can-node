/* SPDX-License-Identifier: MIT */
#include "update.h"

#include <stddef.h>
#include <string.h>

/* The hand-off's values are BMS_BootInfo's (telemetry.c checks them against the DBC) and
 * lib/boot's attempts. */
_Static_assert(PLAT_HANDOFF_MAX_ATTEMPT == BOOT_MAX_ATTEMPTS, "three attempts for a trial");
_Static_assert(UPDATE_CONFIRM_AFTER_S > 0U, "a trial runs before it confirms itself");

bool update_init(update_monitor_t *m, const plat_boot_handoff_t *handoff,
                 uint32_t periods_per_second, uint32_t window_periods)
{
    const bool ok = (m != NULL);

    if (ok) {
        const uint32_t per_second = (periods_per_second == 0U) ? 1U : periods_per_second;

        (void)memset(m, 0, sizeof(*m));
        m->periods_needed =
            (window_periods != 0U) ? window_periods : (UPDATE_CONFIRM_AFTER_S * per_second);
        m->confirm_status = BOOT_OK;
        m->state = UPDATE_CONFIRM_NOT_NEEDED;

        if (handoff != NULL) {
            m->have_handoff = true;
            m->handoff = *handoff;

            if (handoff->mode == PLAT_BOOT_TRIAL) {
                m->state = UPDATE_CONFIRM_WAITING;
            }
        }
    }

    return ok;
}

bool update_health_period(update_monitor_t *m, const update_health_t *health)
{
    bool request = false;

    if (m->state == UPDATE_CONFIRM_WAITING) {
        /* The four conditions of good health (update.h, THE CONFIRM OF A TRIAL). A bus-off
         * between two periods counts although the controller has recovered since, and the
         * frames acknowledged are those since the last period, whatever came in between. */
        const bool supervised = (health->supervision == WDGM_GLOBAL_STATUS_OK);
        const bool active = (health->can_state == CAN_FC_ERROR_ACTIVE);
        const bool bus_off = (health->bus_off_count != m->bus_off_count);
        const bool acked = (health->tx_acked > m->tx_acked);
        const uint64_t new_acked = acked ? (health->tx_acked - m->tx_acked) : 0U;

        m->periods++;

        if (!supervised) {
            m->unsupervised_periods++;
        }

        if (!active) {
            m->not_active_periods++;
        }

        if (bus_off) {
            m->bus_off_periods++;
        }

        if (!acked) {
            m->unacked_periods++;
        }

        if (supervised && active && !bus_off && acked) {
            m->healthy_periods++;
            m->window_acked += new_acked;

            if ((m->fewest_acked == 0U) || (new_acked < m->fewest_acked)) {
                m->fewest_acked = new_acked;
            }
        } else {
            if (m->healthy_periods != 0U) {
                m->window_restarts++;
            }

            m->healthy_periods = 0U;
            m->window_acked = 0U;
            m->fewest_acked = 0U;
        }

        if (m->healthy_periods >= m->periods_needed) {
            m->state = UPDATE_CONFIRM_CONFIRMING;
            m->requested_period = m->periods;
            request = true;
        }
    }

    m->bus_off_count = health->bus_off_count;
    m->tx_acked = health->tx_acked;
    return request;
}

void update_confirm_done(update_monitor_t *m, boot_status_t status)
{
    if (m->state == UPDATE_CONFIRM_CONFIRMING) {
        m->confirm_status = status;
        m->state = (status == BOOT_OK) ? UPDATE_CONFIRM_CONFIRMED : UPDATE_CONFIRM_FAILED;
    }
}

void update_routine_confirmed(update_monitor_t *m)
{
    if ((m->state == UPDATE_CONFIRM_WAITING) || (m->state == UPDATE_CONFIRM_CONFIRMING)) {
        m->state = UPDATE_CONFIRM_CONFIRMED;
        m->confirm_status = BOOT_OK;
        m->by_routine = true;
    }
}

uint32_t update_healthy_seconds(const update_monitor_t *m, uint32_t periods_per_second)
{
    const uint32_t per_second = (periods_per_second == 0U) ? 1U : periods_per_second;

    return m->healthy_periods / per_second;
}

bool update_rolled_back(const plat_boot_handoff_t *handoff)
{
    bool rolled_back = false;

    if (handoff != NULL) {
        rolled_back = (handoff->event == PLAT_BOOT_EVENT_ROLLBACK) ||
                      (handoff->event == PLAT_BOOT_EVENT_REJECT) ||
                      (handoff->event == PLAT_BOOT_EVENT_FALLBACK);
    }

    return rolled_back;
}

const char *update_confirm_state_name(update_confirm_state_t state)
{
    static const char *const names[] = {"NOT_NEEDED", "WAITING", "CONFIRMING", "CONFIRMED",
                                        "FAILED"};
    const char *name = "unknown";

    const uint32_t index = (uint32_t)state;

    if (index < (uint32_t)(sizeof(names) / sizeof(names[0]))) {
        name = names[index];
    }

    return name;
}

const char *update_event_name(plat_boot_event_t event)
{
    const char *name;

    switch (event) {
    case PLAT_BOOT_EVENT_NONE:
        name = "NONE";
        break;
    case PLAT_BOOT_EVENT_ADOPT:
        name = "ADOPT";
        break;
    case PLAT_BOOT_EVENT_ROLLBACK:
        name = "ROLLBACK";
        break;
    case PLAT_BOOT_EVENT_REJECT:
        name = "REJECT";
        break;
    case PLAT_BOOT_EVENT_FALLBACK:
        name = "FALLBACK";
        break;
    default:
        name = "unknown";
        break;
    }

    return name;
}

bool update_boot_config(const plat_flash_layout_t *layout, const flash_dev_t *dev,
                        const boot_validator_t *validator, boot_config_t *out)
{
    const bool ok = (layout != NULL) && (dev != NULL) && (out != NULL);

    if (ok) {
        (void)memset(out, 0, sizeof(*out));
        out->dev = dev;

        if (validator != NULL) {
            out->validator = *validator;
        }

        out->meta_addr = layout->meta.offset;
        out->meta_sectors = layout->meta.size / layout->geometry.sector_size;

        for (uint32_t s = 0U; s < BOOT_SLOTS; s++) {
            out->slot[s].addr = layout->slot[s].offset;
            out->slot[s].size = layout->slot[s].size;
        }
    }

    return ok;
}
