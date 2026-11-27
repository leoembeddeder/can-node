/* SPDX-License-Identifier: MIT */
/**
 * @file
 * The watchdog manager: alive supervision of the tasks that must not stop, and the decision
 * whether the hardware watchdog may still be kicked.
 *
 * Source: AUTOSAR "Specification of Watchdog Manager" (AUTOSAR_SWS_WatchdogManager, CP
 * R22-11), the Alive Supervision of section 7.2.1 and the Local and Global Supervision Status of
 * section 7.1 (Figures 2 and 3). Requirement numbers are quoted where a rule is implemented.
 *
 * WHY. A hardware watchdog only knows whether somebody kicks it. If the one task that kicks it
 * keeps running while another task hangs, the hardware watchdog sees nothing wrong. So every
 * task that must not stop reports that it is alive (a checkpoint), one supervising task checks
 * the reports at a fixed period, and only that task kicks the hardware watchdog, and only while
 * the reports are good enough. Once they are not, it stops kicking and the hardware watchdog
 * resets the node. The supervising task itself is covered by the hardware watchdog: if it
 * stops, nobody kicks.
 *
 * SUPERVISED ENTITIES. The caller describes each entity in a table (wdgm_entity_config_t); its
 * index in the table is its identifier (the SEID). An entity has one checkpoint, as in the
 * simplest configuration of Alive Supervision (SWS section 7.2.1.1, Figure 4): a task that
 * reports wdgm_checkpoint() once per loop is one entity.
 *
 * ALIVE SUPERVISION. wdgm_checkpoint() adds one to the entity's alive counter (SWS_WdgM_00321).
 * The caller runs wdgm_main_function() once per supervision cycle, its period. Every
 * reference_cycle-th call examines each entity's counter (SWS_WdgM_00098; in the cycles in
 * between nothing is examined) and starts it again from 0. The counted indications are correct
 * when
 *
 *     expected_alive - min_margin  <=  count  <=  expected_alive + max_margin
 *
 * (SWS_WdgM_00074, SWS_WdgM_00115), and incorrect otherwise. The first examination comes at the
 * reference_cycle-th call after wdgm_init() and counts everything reported since then.
 *
 * LOCAL SUPERVISION STATUS of an entity, after each of its examinations (SWS 7.1.2, Figure 2).
 * F is the counter of failed reference cycles, T the entity's failed_tolerance
 * (WdgMFailedAliveSupervisionRefCycleTol):
 *
 *   from     result      condition   to        F         requirement
 *   OK       correct                 OK                  SWS_WdgM_00201
 *   OK       incorrect   T == 0      EXPIRED             SWS_WdgM_00202
 *   OK       incorrect   T > 0       FAILED    1         SWS_WdgM_00203
 *   FAILED   incorrect   F < T       FAILED    F + 1     SWS_WdgM_00204
 *   FAILED   incorrect   F == T      EXPIRED             SWS_WdgM_00206
 *   FAILED   correct     F > 1       FAILED    F - 1     SWS_WdgM_00300
 *   FAILED   correct     F == 1      OK        0         SWS_WdgM_00205
 *   EXPIRED  any                     EXPIRED             (WdgM_LocalStatusType: "This state
 *                                                         cannot be left")
 *
 * So an entity is allowed T failed reference cycles, net of the good ones in between, and the
 * (T + 1)-th makes it EXPIRED. An entity that stops reporting altogether gives incorrect
 * examinations from the first one whose window holds none of its indications (the lower bound
 * is at least 1, see wdgm_entity_config_t), so it is EXPIRED within T + 2 reference cycles of
 * its last indication.
 *
 * GLOBAL SUPERVISION STATUS, after the entities, in every call of wdgm_main_function()
 * (SWS_WdgM_00214, Figure 3). E is the expired cycle counter, X the expired_tolerance
 * (WdgMExpiredSupervisionCycleTol):
 *
 *   from        entities                     to        E       requirement
 *   OK          all OK                       OK                SWS_WdgM_00078
 *   OK          one FAILED, none EXPIRED     FAILED            SWS_WdgM_00076
 *   OK          one EXPIRED, X > 0           EXPIRED   1       SWS_WdgM_00215
 *   OK          one EXPIRED, X == 0          STOPPED           SWS_WdgM_00216
 *   FAILED      one FAILED, none EXPIRED     FAILED            SWS_WdgM_00217
 *   FAILED      all OK                       OK                SWS_WdgM_00218
 *   FAILED      one EXPIRED, X > 0           EXPIRED   1       SWS_WdgM_00077
 *   FAILED      one EXPIRED, X == 0          STOPPED           SWS_WdgM_00117
 *   EXPIRED     (one EXPIRED), E < X         EXPIRED   E + 1   SWS_WdgM_00219
 *   EXPIRED     (one EXPIRED), E == X        STOPPED           SWS_WdgM_00220
 *   STOPPED     any                          STOPPED           SWS_WdgM_00221
 *
 * In EXPIRED the hardware watchdog is still kicked, for X supervision cycles counted from the
 * one in which the entity expired: time for the caller to record the failure before the reset
 * (the SWS's reason for the tolerance, section 7.1.3). With X == 0 the kicking stops in the
 * cycle the entity expired.
 *
 * THE HARDWARE WATCHDOG. wdgm_may_kick() is true in OK, FAILED and EXPIRED and false in
 * STOPPED (SWS_WdgM_00223 items 1 and 2, SWS_WdgM_00122: trigger condition 0), and false in
 * DEACTIVATED too: SWS_WdgM_00223 item 3 leaves the trigger condition alone there, but a
 * manager that was never initialised must not keep a hardware watchdog alive, so here it does
 * not allow a kick. The caller kicks the hardware watchdog after each wdgm_main_function()
 * call that allows it and at no other time.
 *
 * WHERE THIS MODULE IS STRICTER THAN THE SWS.
 *   - A configuration whose lower bound expected_alive - min_margin is 0 is refused: it would
 *     accept an examination without a single indication, so an entity that never runs again
 *     would never be found out.
 *   - wdgm_main_function() checks the configuration and the state before it uses them. A
 *     configuration that is not valid or not the size given to wdgm_init(), or a state that no
 *     sequence of calls can produce (a status value outside the enumeration, a counter beyond
 *     its limit), makes the global status STOPPED: a watchdog manager that cannot trust its own
 *     data lets the hardware watchdog reset the node, as the SWS does when it cannot set the
 *     watchdog's mode (SWS_WdgM_00139).
 *
 * WHAT IS LEFT OUT. Deadline and Logical Supervision; modes (WdgM_SetMode) and with them the
 * activation and deactivation of entities, so that an entity is DEACTIVATED only before
 * wdgm_init(); several checkpoints per entity; WdgM_PerformReset; reporting to the DEM (the
 * caller records the failure, for example as a trouble code); several main function instances;
 * the watchdog interface (WdgIf_SetTriggerCondition): the caller asks wdgm_may_kick() and kicks
 * the watchdog itself.
 *
 * NUMBERING. The SWS gives WDGM_LOCAL_STATUS_OK and WDGM_GLOBAL_STATUS_OK the value 0. Here 0 is
 * DEACTIVATED for both, so that a state in zeroed static memory reads as not initialised, the
 * value SWS_WdgM_00387 gives the global status before WdgM_Init, and wdgm_may_kick() refuses it.
 *
 * CONCURRENCY.
 *   - wdgm_checkpoint() may be called from any number of tasks and interrupt handlers at the
 *     same time, and at the same time as wdgm_main_function(). It is lock-free: one atomic
 *     fetch-and-add on the entity's counter, the only datum it writes. The examination takes
 *     the count with an atomic exchange for 0, so every indication is counted exactly once,
 *     by the examination after it. Relaxed ordering is enough: the counter is the only thing
 *     handed over. The build needs lock-free 32-bit atomics (a compile-time check below); a
 *     Cortex-M3 or M4 has them (LDREX/STREX), a Cortex-M0 does not.
 *   - Everything else belongs to one task, the supervising task: wdgm_init() before any
 *     checkpoint is reported (before the scheduler starts, or before the reporting tasks are
 *     created), then wdgm_main_function() and the read functions. Another task that wants the
 *     statuses gets them from the supervising task (a copy it publishes); reading the state
 *     directly from another task is a data race. The caller needs no critical section.
 *
 * CONTEXT. The functions keep no data of their own, allocate nothing, read no clock and do not
 * block. The state is caller memory, one per watchdog manager; the configuration is constant.
 */
#ifndef FCN_WDGM_H
#define FCN_WDGM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/** Entities a state has room for. */
#define WDGM_MAX_ENTITIES 8U

/** wdgm_first_expired() before any entity has expired. */
#define WDGM_NO_ENTITY 0xFFU

/** WdgM_LocalStatusType (SWS section 8.2), numbered differently: see NUMBERING. */
typedef enum {
    WDGM_LOCAL_STATUS_DEACTIVATED = 0, /**< not supervised: before wdgm_init(), or no entity */
    WDGM_LOCAL_STATUS_OK,              /**< no failed reference cycle outstanding */
    WDGM_LOCAL_STATUS_FAILED,          /**< failed, within the tolerance; can heal */
    WDGM_LOCAL_STATUS_EXPIRED          /**< failed beyond the tolerance; final */
} wdgm_local_status_t;

/** WdgM_GlobalStatusType (SWS section 8.2), numbered differently: see NUMBERING. */
typedef enum {
    WDGM_GLOBAL_STATUS_DEACTIVATED = 0, /**< not initialised: no supervision, no kick */
    WDGM_GLOBAL_STATUS_OK,              /**< every entity OK */
    WDGM_GLOBAL_STATUS_FAILED,          /**< an entity FAILED, none EXPIRED; kick */
    WDGM_GLOBAL_STATUS_EXPIRED,         /**< an entity EXPIRED; kick for expired_tolerance cycles */
    WDGM_GLOBAL_STATUS_STOPPED          /**< no more kicks: the hardware watchdog will reset */
} wdgm_global_status_t;

/**
 * WdgMAliveSupervision of an entity's checkpoint (SWS section 10, ECUC_WdgM_00311 to 00313)
 * and WdgMFailedAliveSupervisionRefCycleTol (ECUC_WdgM_00327). Constant data.
 *
 * Valid values, checked by wdgm_init() and by every wdgm_main_function():
 * expected_alive >= 1; min_margin < expected_alive (the lower bound is at least 1); any
 * max_margin; reference_cycle >= 1; any failed_tolerance. The bounds are computed in 32 bits,
 * so expected_alive + max_margin cannot overflow.
 */
typedef struct {
    const char *name;         /**< for diagnosis only, may be NULL */
    uint16_t expected_alive;  /**< WdgMExpectedAliveIndications per reference cycle */
    uint16_t min_margin;      /**< WdgMMinMargin: fewer indications that are still correct */
    uint16_t max_margin;      /**< WdgMMaxMargin: more indications that are still correct */
    uint16_t reference_cycle; /**< WdgMSupervisionReferenceCycle, in supervision cycles */
    uint8_t failed_tolerance; /**< failed reference cycles tolerated before EXPIRED */
} wdgm_entity_config_t;

/** The configuration of one watchdog manager. Constant data. */
typedef struct {
    const wdgm_entity_config_t *entities; /**< entity i has the identifier i */
    uint8_t entity_count;                 /**< 1..WDGM_MAX_ENTITIES */
    uint8_t expired_tolerance; /**< WdgMExpiredSupervisionCycleTol (ECUC_WdgM_00329): supervision
                                    cycles in EXPIRED, with kicks, before STOPPED */
} wdgm_config_t;

/** One entity's state. `alive` is the shared counter (see CONCURRENCY); the other members are
 *  the supervising task's and may be read by it for diagnosis. */
typedef struct {
    _Atomic uint32_t alive;     /**< indications since the last examination, modulo 2^32: a
                                       task stuck in a loop that reports 2^32 times within one
                                       reference cycle would be examined as reporting 0 */
    uint32_t last_count;        /**< the count the last examination saw */
    uint32_t examinations;      /**< examinations since wdgm_init() */
    uint32_t incorrect;         /**< of those, the incorrect ones */
    uint16_t cycle;             /**< supervision cycles since the last examination */
    uint8_t failed_cycles;      /**< the counter of failed reference cycles (F above) */
    wdgm_local_status_t status; /**< the Local Supervision Status */
} wdgm_entity_state_t;

/** The state of one watchdog manager: caller memory, zeroed or not before wdgm_init(). */
typedef struct {
    wdgm_entity_state_t entity[WDGM_MAX_ENTITIES];
    uint32_t cycles;             /**< supervision cycles run since wdgm_init(); wraps */
    uint8_t entity_count;        /**< from the configuration given to wdgm_init(); 0 before */
    uint8_t expired_cycles;      /**< the expired cycle counter (E above) */
    uint8_t first_expired;       /**< the first entity that expired, or WDGM_NO_ENTITY */
    wdgm_global_status_t status; /**< the Global Supervision Status */
} wdgm_state_t;

/**
 * WdgM_Init (SWS_WdgM_00151) without modes: every entity OK with its counters at 0
 * (SWS_WdgM_00268), the global status OK with the expired cycle counter at 0
 * (SWS_WdgM_00285), nothing expired yet. The memory of `state` may hold anything before.
 *
 * Returns false if a pointer is NULL or the configuration is not valid (see
 * wdgm_entity_config_t and wdgm_config_t); a non-NULL state is then left DEACTIVATED, so that
 * wdgm_may_kick() is false. The supervising task calls it, before any checkpoint is reported.
 */
bool wdgm_init(wdgm_state_t *state, const wdgm_config_t *config);

/**
 * WdgM_CheckpointReached (SWS_WdgM_00263, SWS_WdgM_00321): one alive indication of `entity`.
 *
 * Returns false and counts nothing if `state` is NULL or `entity` is not an identifier of the
 * configuration given to wdgm_init() (also before wdgm_init()). Any task or interrupt handler,
 * concurrently (see CONCURRENCY). Constant time, lock-free.
 */
bool wdgm_checkpoint(wdgm_state_t *state, uint8_t entity);

/**
 * WdgM_MainFunction (SWS_WdgM_00159): one supervision cycle. Examines every entity whose
 * reference cycle is complete and steps its Local Supervision Status (SWS_WdgM_00324,
 * SWS_WdgM_00325), then steps the Global Supervision Status (SWS_WdgM_00214). Returns the new
 * global status.
 *
 * `config` must be the configuration given to wdgm_init(). Does nothing and returns
 * DEACTIVATED if `state` is NULL or not initialised (SWS_WdgM_00063). Makes the global status
 * STOPPED, examining nothing, if `config` is NULL or not valid, has another number of entities
 * than at wdgm_init(), or the state is damaged (see WHERE THIS MODULE IS STRICTER THAN THE SWS).
 * The supervising task only, once per supervision cycle; time is O(entity_count).
 */
wdgm_global_status_t wdgm_main_function(wdgm_state_t *state, const wdgm_config_t *config);

/** WdgM_GetGlobalStatus (SWS_WdgM_00175). DEACTIVATED for a NULL state. Supervising task. */
wdgm_global_status_t wdgm_global_status(const wdgm_state_t *state);

/** WdgM_GetLocalStatus (SWS_WdgM_00169). DEACTIVATED for a NULL state and for an identifier
 *  that is not one of the configuration's. Supervising task. */
wdgm_local_status_t wdgm_local_status(const wdgm_state_t *state, uint8_t entity);

/** Whether the hardware watchdog may be kicked now: true in OK, FAILED and EXPIRED, false in
 *  STOPPED and DEACTIVATED (see THE HARDWARE WATCHDOG). Supervising task. */
bool wdgm_may_kick(const wdgm_state_t *state);

/** WdgM_GetFirstExpiredSEID (SWS_WdgM_00346): the entity that expired first, the lowest
 *  identifier if several expired in the same cycle; WDGM_NO_ENTITY while none has, and for a
 *  NULL or not initialised state. Supervising task. */
uint8_t wdgm_first_expired(const wdgm_state_t *state);

#endif /* FCN_WDGM_H */
