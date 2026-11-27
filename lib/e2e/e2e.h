/* SPDX-License-Identifier: MIT */
/**
 * @file
 * What the functions of lib/e2e return: the error codes of the AUTOSAR E2E library
 * (AUTOSAR_CP_SWS_E2ELibrary R24-11, SWS_E2E_00047), with the library's numeric values so
 * that a trace reads the same as one of an AUTOSAR stack.
 *
 * An error code reports a mistake of the caller or a damaged state, never a communication
 * fault: what the check of a received message found is its status (e2e_p11_status_t), which
 * is only meaningful when the call returned E2E_E_OK.
 *
 * The profile is in e2e/e2e_p11.h, the state machine that judges a sequence of checks in
 * e2e/e2e_sm.h.
 */
#ifndef FCN_E2E_H
#define FCN_E2E_H

/** Std_ReturnType values of the E2E library functions (SWS_E2E_00047). */
typedef enum {
    E2E_E_OK = 0x00,             /**< the function did its work */
    E2E_E_INPUTERR_NULL = 0x13,  /**< a pointer parameter was NULL; nothing was changed */
    E2E_E_INPUTERR_WRONG = 0x17, /**< a parameter, the configuration or the state passed in
                                      was out of range; nothing was changed */
    E2E_E_WRONGSTATE = 0x1A      /**< the state machine was used before its initialisation */
} e2e_result_t;

#endif /* FCN_E2E_H */
