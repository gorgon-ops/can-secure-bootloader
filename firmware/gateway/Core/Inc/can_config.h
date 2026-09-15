/**
 * @file    can_config.h
 * @brief   Compile-time selection of bus topology and what the node runs.
 *
 * Two independent switches. Override either in the project symbols (CubeIDE:
 * Project > Properties > C/C++ Build > Settings > Preprocessor) or on the
 * command line, as tools/build_firmware.sh does.
 */
#ifndef CAN_CONFIG_H
#define CAN_CONFIG_H

/* --- Bus topology ------------------------------------------------------- */

/** CAN1 in silent-loopback. No transceiver, no wiring, no second node. */
#define CAN_BRINGUP_LOOPBACK 0

/** CAN1 and CAN2 on one physical bus, each behind its own transceiver. */
#define CAN_BRINGUP_DUAL 1

#ifndef CAN_BRINGUP_MODE
#define CAN_BRINGUP_MODE CAN_BRINGUP_LOOPBACK
#endif

/* --- What the main loop runs -------------------------------------------- */

/** Raw diagnostic frames: fixed payload on a fixed ID, no protocol. */
#define BFMC_RUN_BRINGUP 0

/** The vehicle application: DBC-defined frames through the safety logic. */
#define BFMC_RUN_APPLICATION 1

/**
 * Defaults to the bring-up harness.
 *
 * Bring-up answers "is this bus alive at all", and that question has to be
 * settled before the application is worth running: a silent bus and a wedged
 * application look identical from the outside otherwise.
 */
#ifndef BFMC_RUN_MODE
#define BFMC_RUN_MODE BFMC_RUN_BRINGUP
#endif

#endif /* CAN_CONFIG_H */
