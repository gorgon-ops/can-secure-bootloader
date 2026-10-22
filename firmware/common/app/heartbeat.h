/* A node silent for three periods is lost. Heartbeats carry a counter, like
 * MOTION_CMD, because a wedged node can keep a timer interrupt publishing on
 * its behalf: a frozen counter is STALE, not ALIVE. */
#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "node_state.h"

#include <stdbool.h>
#include <stdint.h>

#define HEARTBEAT_MAX_NODES 8U

typedef enum
{
    HEARTBEAT_ABSENT = 0,
    HEARTBEAT_ALIVE,
    HEARTBEAT_LOST,
    HEARTBEAT_STALE       /* arriving, but the counter is frozen */
} heartbeat_status_t;

typedef struct
{
    bool     registered;
    bool     required;      /* a missing optional node is not a fault */
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

void Heartbeat_Init(heartbeat_monitor_t *mon, uint32_t timeout_ms);

/* Only registered nodes are supervised; node_id is the DBC NodeAddress. */
void Heartbeat_Register(heartbeat_monitor_t *mon, uint8_t node_id, bool required);

void Heartbeat_OnFrame(heartbeat_monitor_t *mon,
                       uint8_t node_id,
                       uint8_t counter,
                       uint32_t now_ms);

/* Bitmask of required nodes not ALIVE - shaped to drop straight into the
 * FaultFlags signal of SAFETY_STATE. */
uint32_t Heartbeat_Poll(heartbeat_monitor_t *mon, uint32_t now_ms);

heartbeat_status_t Heartbeat_Status(const heartbeat_monitor_t *mon, uint8_t node_id);
bool Heartbeat_AllRequiredAlive(const heartbeat_monitor_t *mon);
node_state_t Heartbeat_AggregateState(const heartbeat_monitor_t *mon);
const char *Heartbeat_StatusName(heartbeat_status_t status);

#endif
