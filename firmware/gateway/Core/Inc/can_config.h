#ifndef CAN_CONFIG_H
#define CAN_CONFIG_H

/* --- Bus topology ------------------------------------------------------- */

#define CAN_BRINGUP_LOOPBACK 0

#define CAN_BRINGUP_DUAL 1

#ifndef CAN_BRINGUP_MODE
#define CAN_BRINGUP_MODE CAN_BRINGUP_LOOPBACK
#endif

/* --- Transceiver logic levels -------------------------------------------- */

/* Open-drain CAN_TX for 5 V transceivers (MCP2551, TJA1050), whose TXD high
 * threshold of 3.5 V is above what a 3.3 V pin can drive.
 *
 * REQUIRES an external 1k..4k7 pull-up to +5 V on every CAN_TX pin. Without it
 * TXD is stuck dominant and the node jams the whole bus, hence off by default. */
#ifndef CAN_TX_OPEN_DRAIN
#define CAN_TX_OPEN_DRAIN 0
#endif

/* --- What the main loop runs -------------------------------------------- */

#define BFMC_RUN_BRINGUP 0

#define BFMC_RUN_APPLICATION 1

/* Bring-up first: a silent bus and a wedged application look identical from
 * the outside. */
#ifndef BFMC_RUN_MODE
#define BFMC_RUN_MODE BFMC_RUN_BRINGUP
#endif

/* --- Watchdog ------------------------------------------------------------ */

/* Off for bring-up, where a board resetting itself while you read its counters
 * is worse than useless. */
#ifndef BFMC_WATCHDOG_ENABLE
#if BFMC_RUN_MODE == BFMC_RUN_APPLICATION
#define BFMC_WATCHDOG_ENABLE 1
#else
#define BFMC_WATCHDOG_ENABLE 0
#endif
#endif

/* --- Fault handling ------------------------------------------------------ */

/* A reproducible fault would otherwise reboot forever. After this many with no
 * healthy run in between, the failure path parks instead. */
#ifndef BFMC_FAULT_RESET_LIMIT
#define BFMC_FAULT_RESET_LIMIT 3U
#endif

#endif
