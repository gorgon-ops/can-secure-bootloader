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

/* --- Transceiver logic levels -------------------------------------------- */

/**
 * Drive CAN_TX open-drain instead of push-pull, for a 5 V transceiver.
 *
 * The MCP2551 and TJA1050 are 5 V parts. Their TXD input specifies a logic high
 * of 0.7 x VDD, which is **3.5 V at a 5.0 V supply** -- above anything a 3.3 V
 * STM32 pin can produce (V_OH is about 3.2 V). The receive direction is fine,
 * because PA11 and PB12 are five-volt tolerant, so the failure is asymmetric
 * and confusing: the node receives other traffic perfectly while its own
 * transmissions are ignored or intermittent.
 *
 * Open-drain fixes it with one resistor and no level-shifter IC. The pin only
 * ever pulls low; an external pull-up to +5 V provides the high level, so TXD
 * sees a true 5 V recessive state. This suits CAN especially well, since the
 * protocol is already dominant-low / recessive-high.
 *
 * REQUIRES an external pull-up on every CAN_TX pin you enable this for:
 *
 *     PA12 (CAN1_TX) --+-- 1k..4k7 --> +5 V
 *     PB13 (CAN2_TX) --+-- 1k..4k7 --> +5 V
 *
 * Without that resistor the line can never go high, TXD is stuck dominant, and
 * the node jams the entire bus. That is why this defaults to off: the safe
 * configuration is the one that needs no external parts.
 *
 * 1 k with a few pF of load settles in tens of nanoseconds, comfortably inside
 * the 2 us bit time at 500 kbit/s. A 3.3 V transceiver such as the SN65HVD230
 * does not need this at all -- leave it off and use push-pull.
 */
#ifndef CAN_TX_OPEN_DRAIN
#define CAN_TX_OPEN_DRAIN 0
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

/* --- Watchdog ------------------------------------------------------------ */

/**
 * Enable the independent watchdog.
 *
 * Defaults on for the application and off for bring-up. Bring-up exists to
 * diagnose a bus that does not work, and a board that resets itself every
 * quarter second while you are reading its counters is worse than useless.
 * The application is the build that should never hang silently.
 */
#ifndef BFMC_WATCHDOG_ENABLE
#if BFMC_RUN_MODE == BFMC_RUN_APPLICATION
#define BFMC_WATCHDOG_ENABLE 1
#else
#define BFMC_WATCHDOG_ENABLE 0
#endif
#endif

/* --- Fault handling ------------------------------------------------------ */

/**
 * Consecutive Error_Handler() faults before the board stops rebooting.
 *
 * A transient fault is best cleared by a reset. A reproducible one is not: it
 * would reboot forever, which looks identical from outside and is nearly
 * impossible to attach a debugger to. After this many faults with no healthy
 * run in between, the failure path parks instead.
 */
#ifndef BFMC_FAULT_RESET_LIMIT
#define BFMC_FAULT_RESET_LIMIT 3U
#endif

#endif /* CAN_CONFIG_H */
