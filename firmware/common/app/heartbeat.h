/**
 * Heartbeat supervision.
 *
 * Each node publishes its own heartbeat ID every APP_HEARTBEAT_PERIOD_MS. A
 * consumer registers the nodes it depends on and polls this module; a node that
 * goes quiet for three periods is declared lost, and the consumer drops to
 * NODE_STATE_DEGRADED.
 *
 * Heartbeats carry a counter for the same reason MOTION_CMD does: a sender can
 * be wedged while a timer interrupt keeps publishing on its behalf. A frozen
 * counter is reported as STALE rather than ALIVE.
 *
 * Pure module: no globals, no I/O, no HAL. Time comes in as a parameter.
 */
#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "node_state.h"

#include <stdbool.h>
#include <stdint.h>

/** Enough for the four nodes in the architecture plus headroom. */
#define HEARTBEAT_MAX_NODES 8U

typedef enum
{
    HEARTBEAT_ABSENT = 0, /*!< never heard from since startup            */
    HEARTBEAT_ALIVE,      /*!< heartbeats arriving, counter advancing    */
    HEARTBEAT_LOST,       /*!< silent past the timeout budget            */
    HEARTBEAT_STALE       /*!< arriving, but the counter is frozen       */
} heartbeat_status_t;

typedef struct
{
    bool     registered;
    bool     required;      /*!< a missing optional node is not a fault  */
    bool     seeded;
    uint8_t  last_counter;
    uint32_t last_rx_ms;
    uint32_t last_fresh_ms;
    heartbeat_status_t status;
} heartbeat_node_t;

typedef struct
{
    heartbeat_node_t nodes[HEARTBEAT_MAX_NODES];
    uint32_t timeout_ms;
} heartbeat_monitor_t;

/**
 * @brief Reset the supervisor and set the loss budget.
 * @param timeout_ms e.g. APP_HEARTBEAT_TIMEOUT_MS
 */
void Heartbeat_Init(heartbeat_monitor_t *mon, uint32_t timeout_ms);

/**
 * @brief Declare interest in a node.
 * @param node_id  index below HEARTBEAT_MAX_NODES; use the DBC NodeAddress
 * @param required true if losing this node is a fault for this consumer
 *
 * Only registered nodes are supervised, so each ECU watches exactly the peers
 * it depends on instead of every node on the bus.
 */
void Heartbeat_Register(heartbeat_monitor_t *mon, uint8_t node_id, bool required);

/** @brief Feed a received heartbeat frame. Unregistered ids are ignored. */
void Heartbeat_OnFrame(heartbeat_monitor_t *mon,
                       uint8_t node_id,
                       uint8_t counter,
                       uint32_t now_ms);

/**
 * @brief Re-evaluate every registered node against the clock.
 * @return bitmask of required nodes that are not ALIVE, bit n for node n.
 *
 * The mask is shaped to drop straight into the FaultFlags signal of
 * SAFETY_STATE, so the wire representation and the internal one cannot drift.
 */
uint32_t Heartbeat_Poll(heartbeat_monitor_t *mon, uint32_t now_ms);

/** @brief Status of one node as of the last poll. */
heartbeat_status_t Heartbeat_Status(const heartbeat_monitor_t *mon, uint8_t node_id);

/** @brief True when every required node was ALIVE at the last poll. */
bool Heartbeat_AllRequiredAlive(const heartbeat_monitor_t *mon);

/**
 * @brief Node state implied by peer health.
 * @return NODE_STATE_RUNNING when all required peers are alive,
 *         NODE_STATE_DEGRADED otherwise.
 */
node_state_t Heartbeat_AggregateState(const heartbeat_monitor_t *mon);

/** @brief Human-readable status, for diagnostics and test output. */
const char *Heartbeat_StatusName(heartbeat_status_t status);

#endif /* HEARTBEAT_H */
