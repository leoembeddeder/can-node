#!/usr/bin/env bash
# The regression test added with the fix: the unit test of the locked tick's decisions
# (tests/platform/test_posix_tick_plan.c), independent of the host's timing. A model of the
# port's tick thread and of the kernel plays orders of events through posix_tick_plan.c; in
# one of them a tick taken more than a period late has been made up in the meantime, and every
# other raise is taken just after the plan has read the count, as on the development laptop.
# The test fails if, after the late tick, more than one tick is taken before it was due: with
# the give-back of bug 011 the count stays one ahead, and every tick after it is early.
"$BUILD/tests/platform/test_posix_tick_plan"
