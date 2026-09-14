/**
 * Node lifecycle states.
 *
 * These values are wire-visible: they are transmitted in the NodeState signal
 * of every heartbeat frame, and the numbering must match the VAL_ table in
 * dbc/bfmc.dbc. The static assertions in the tests guard that, since a silent
 * divergence here would make one node misreport its health to every other.
 */
#ifndef NODE_STATE_H
#define NODE_STATE_H

typedef enum
{
    NODE_STATE_INIT     = 0, /*!< starting up, not yet participating        */
    NODE_STATE_RUNNING  = 1, /*!< healthy, all dependencies present         */
    NODE_STATE_DEGRADED = 2, /*!< operating, but a dependency is missing    */
    NODE_STATE_BUS_OFF  = 3, /*!< CAN controller has gone bus-off           */
    NODE_STATE_FAULT    = 4  /*!< unrecoverable local fault                 */
} node_state_t;

/** @brief Human-readable state, for diagnostics and test output. */
const char *NodeState_Name(node_state_t state);

#endif /* NODE_STATE_H */
