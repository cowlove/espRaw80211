#pragma once

// ARTIFICIAL TEST KNOWLEDGE ONLY. A real swarm has no known global board count.
// This parameter is an external test oracle for convergence/reset experiments,
// NEVER an input to discovery, rendezvous scheduling, or membership acceptance.
// Count ALL participating boards, including battery-powered/unlogged boards.
// USB flashing/logging connections and CSIM context count are independent.
// The offline analyzer reads this same definition; edit here for the next run.
#define ARTIFICIAL_TEST_SWARM_BOARD_COUNT 7
static_assert(ARTIFICIAL_TEST_SWARM_BOARD_COUNT > 0, "Test swarm must be nonempty");
