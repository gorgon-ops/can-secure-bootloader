#include "heartbeat.h"
#include "app_time.h"

#include <string.h>

const char *NodeState_Name(node_state_t state)
{
    switch (state)
    {
        case NODE_STATE_INIT:     return "INIT";
        case NODE_STATE_RUNNING:  return "RUNNING";
        case NODE_STATE_DEGRADED: return "DEGRADED";
        case NODE_STATE_BUS_OFF:  return "BUS_OFF";
        case NODE_STATE_FAULT:    return "FAULT";
        default:                  return "?";
    }
}

void Heartbeat_Init(heartbeat_monitor_t *mon, uint32_t timeout_ms)
{
    memset(mon, 0, sizeof(*mon));
    mon->timeout_ms = timeout_ms;
}

void Heartbeat_Register(heartbeat_monitor_t *mon, uint8_t node_id, bool required)
{
    if (node_id >= HEARTBEAT_MAX_NODES)
    {
        return;
    }

    mon->nodes[node_id].registered = true;
    mon->nodes[node_id].required   = required;
    mon->nodes[node_id].status     = HEARTBEAT_ABSENT;
}

void Heartbeat_OnFrame(heartbeat_monitor_t *mon,
                       uint8_t node_id,
                       uint8_t counter,
                       uint32_t now_ms)
{
    heartbeat_node_t *node;

    if (node_id >= HEARTBEAT_MAX_NODES)
    {
        return;
    }

    node = &mon->nodes[node_id];
    if (!node->registered)
    {
        /* Traffic from a node this consumer does not depend on. Ignoring it
           keeps an unexpected extra node on the bus from affecting our state. */
        return;
    }

    if (!node->seeded)
    {
        node->seeded        = true;
        node->last_counter  = counter;
        node->last_rx_ms    = now_ms;
        node->last_fresh_ms = now_ms;
        return;
    }

    node->last_rx_ms = now_ms;

    if (counter != node->last_counter)
    {
        node->last_counter  = counter;
        node->last_fresh_ms = now_ms;
    }
}

uint32_t Heartbeat_Poll(heartbeat_monitor_t *mon, uint32_t now_ms)
{
    uint32_t fault_mask = 0U;
    uint8_t i;

    for (i = 0U; i < HEARTBEAT_MAX_NODES; i++)
    {
        heartbeat_node_t *node = &mon->nodes[i];

        if (!node->registered)
        {
            continue;
        }

        if (!node->seeded)
        {
            node->status = HEARTBEAT_ABSENT;
        }
        else if (AppTime_Expired(node->last_rx_ms, now_ms, mon->timeout_ms))
        {
            node->status = HEARTBEAT_LOST;
        }
        else if (AppTime_Expired(node->last_fresh_ms, now_ms, mon->timeout_ms))
        {
            node->status = HEARTBEAT_STALE;
        }
        else
        {
            node->status = HEARTBEAT_ALIVE;
        }

        if (node->required && (node->status != HEARTBEAT_ALIVE))
        {
            fault_mask |= (1UL << i);
        }
    }

    return fault_mask;
}

heartbeat_status_t Heartbeat_Status(const heartbeat_monitor_t *mon, uint8_t node_id)
{
    if (node_id >= HEARTBEAT_MAX_NODES)
    {
        return HEARTBEAT_ABSENT;
    }

    return mon->nodes[node_id].status;
}

bool Heartbeat_AllRequiredAlive(const heartbeat_monitor_t *mon)
{
    uint8_t i;

    for (i = 0U; i < HEARTBEAT_MAX_NODES; i++)
    {
        const heartbeat_node_t *node = &mon->nodes[i];

        if (node->registered && node->required && (node->status != HEARTBEAT_ALIVE))
        {
            return false;
        }
    }

    return true;
}

node_state_t Heartbeat_AggregateState(const heartbeat_monitor_t *mon)
{
    return Heartbeat_AllRequiredAlive(mon) ? NODE_STATE_RUNNING
                                           : NODE_STATE_DEGRADED;
}

const char *Heartbeat_StatusName(heartbeat_status_t status)
{
    switch (status)
    {
        case HEARTBEAT_ABSENT: return "ABSENT";
        case HEARTBEAT_ALIVE:  return "ALIVE";
        case HEARTBEAT_LOST:   return "LOST";
        case HEARTBEAT_STALE:  return "STALE";
        default:               return "?";
    }
}
