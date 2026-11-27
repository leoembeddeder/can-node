/* SPDX-License-Identifier: MIT */
/**
 * @file
 * A UDP bus of its own for every test that needs one (shared by the tests in this directory).
 */
#ifndef FCN_TEST_BUS_PORTS_H
#define FCN_TEST_BUS_PORTS_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "can_backend.h"
#include "unity.h"

/* True if nothing is bound to any of the bus's 16 ports. */
static inline bool test_bus_ports_are_free(uint16_t base)
{
    bool all_free = true;

    for (uint16_t slot = 0U; (slot < CAN_UDP_SLOTS) && all_free; slot++) {
        struct sockaddr_in address;
        const int probe = socket(AF_INET, SOCK_DGRAM, 0);

        (void)memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons((uint16_t)(base + slot));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        all_free =
            (probe >= 0) && (bind(probe, (const struct sockaddr *)&address, sizeof(address)) == 0);

        if (probe >= 0) {
            (void)close(probe);
        }
    }

    return all_free;
}

/*
 * The base port of an unused block of 16 ports, searched from a start that differs between
 * processes so that parallel runs rarely even meet. The range lies above the blocks the
 * Python tools hand out (tools/fcn_tools/ports.py) and below Linux's ephemeral ports (32768
 * and up by default), so a socket that the kernel gives a port to can never sit in a slot of
 * a bus under test. Fails the running test if no block is free.
 */
static inline uint16_t test_bus_fresh_base_port(void)
{
    enum { FIRST = 29600, BLOCKS = 190 };
    static unsigned int next_block;
    uint16_t base = 0U;

    if (next_block == 0U) {
        next_block = 1U + ((unsigned int)getpid() % BLOCKS);
    }

    for (unsigned int tries = 0U; (tries < BLOCKS) && (base == 0U); tries++) {
        const uint16_t candidate = (uint16_t)(FIRST + ((next_block % BLOCKS) * CAN_UDP_SLOTS));

        next_block++;

        if (test_bus_ports_are_free(candidate)) {
            base = candidate;
        }
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(0U, base, "no free block of UDP ports for a test bus");
    return base;
}

#endif /* FCN_TEST_BUS_PORTS_H */
