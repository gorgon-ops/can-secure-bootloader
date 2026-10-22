/* Wire-visible in every heartbeat: the numbering must match the VAL_ table in
 * dbc/bfmc.dbc. */
#ifndef NODE_STATE_H
#define NODE_STATE_H

typedef enum
{
    NODE_STATE_INIT     = 0,
    NODE_STATE_RUNNING  = 1,
    NODE_STATE_DEGRADED = 2, /* operating, but a dependency is missing */
    NODE_STATE_BUS_OFF  = 3,
    NODE_STATE_FAULT    = 4
} node_state_t;

const char *NodeState_Name(node_state_t state);

#endif
