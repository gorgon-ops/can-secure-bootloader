/**
 * @file    bfmc_app.c
 * @brief   Gateway and Motion node roles. See bfmc_app.h.
 */
#include "bfmc_app.h"
#include "can_config.h"
#include "can_if.h"
#include "critical_section.h"

#include "app_config.h"
#include "bfmc.h"          /* generated from dbc/bfmc.dbc */

#include <string.h>

volatile bfmc_app_state_t g_bfmc_app;

/* --- Role to controller mapping ----------------------------------------- */

#if CAN_BRINGUP_MODE == CAN_BRINGUP_DUAL
#define GATEWAY_PORT CAN_IF_PORT_1
#define MOTION_PORT  CAN_IF_PORT_2
#else
/* Loopback: one controller hears itself, so both roles live on it. */
#define GATEWAY_PORT CAN_IF_PORT_1
#define MOTION_PORT  CAN_IF_PORT_1
#endif

/* --- Inboxes ------------------------------------------------------------ *
 *
 * Frames are unpacked in the RX interrupt but consumed in the periodic task,
 * so each one lands in a single-slot mailbox first. The task copies the slot
 * with interrupts masked: without that, an arriving frame could overwrite half
 * a command between the task reading the counter and reading the payload,
 * producing a torn value that belongs to neither frame.
 */

typedef struct
{
    volatile uint8_t  pending;
    volatile uint8_t  counter;
    volatile int16_t  speed;
    volatile int16_t  steer;
    volatile uint32_t rx_ms;
} cmd_inbox_t;

typedef struct
{
    volatile uint8_t  pending;
    volatile uint8_t  counter;
    volatile uint32_t rx_ms;
} hb_inbox_t;

static cmd_inbox_t s_cmd_inbox;
static hb_inbox_t  s_hb_inbox[HEARTBEAT_MAX_NODES];

/* --- Role state --------------------------------------------------------- */

static heartbeat_monitor_t s_gw_peers;   /* Gateway watches Motion */
static heartbeat_monitor_t s_mo_peers;   /* Motion watches Gateway */

static cmd_monitor_t  s_cmd_monitor;
static motion_safety_t s_motion_out;

static uint32_t s_last_cmd_tx_ms;
static uint32_t s_last_status_tx_ms;
static uint32_t s_last_hb_tx_ms;
static uint32_t s_last_control_ms;

static uint8_t s_hb_counter;

/* --- Receive path (interrupt context) ------------------------------------ */

static void BfmcApp_OnFrame(can_if_port_t port,
                            uint32_t id,
                            const uint8_t *data,
                            uint8_t dlc)
{
    uint32_t now = HAL_GetTick();

    (void)port; /* routing is by ID; in loopback both roles share a port */

    switch (id)
    {
        case BFMC_MOTION_CMD_FRAME_ID:
        {
            struct bfmc_motion_cmd_t cmd;

            if (bfmc_motion_cmd_unpack(&cmd, data, dlc) != 0)
            {
                g_bfmc_app.decode_errors++;
                break;
            }

            if (s_cmd_inbox.pending != 0U)
            {
                /* The previous command was never consumed. One slot is enough
                   at 50 Hz against a main loop running far faster, so this
                   climbing means the loop has stalled -- worth knowing, since
                   the supervision logic would otherwise just see a gap. */
                g_bfmc_app.cmd_overruns++;
            }

            s_cmd_inbox.counter = cmd.cmd_counter;
            s_cmd_inbox.speed   = cmd.target_speed;
            s_cmd_inbox.steer   = cmd.target_steer_angle;
            s_cmd_inbox.rx_ms   = now;
            s_cmd_inbox.pending = 1U;
            break;
        }

        case BFMC_HB_GATEWAY_FRAME_ID:
        {
            struct bfmc_hb_gateway_t hb;

            if (bfmc_hb_gateway_unpack(&hb, data, dlc) != 0)
            {
                g_bfmc_app.decode_errors++;
                break;
            }

            if (s_hb_inbox[BFMC_NODE_GATEWAY].pending != 0U)
            {
                g_bfmc_app.hb_overruns++;
            }
            s_hb_inbox[BFMC_NODE_GATEWAY].counter = hb.gateway_hb_counter;
            s_hb_inbox[BFMC_NODE_GATEWAY].rx_ms   = now;
            s_hb_inbox[BFMC_NODE_GATEWAY].pending = 1U;
            break;
        }

        case BFMC_HB_MOTION_FRAME_ID:
        {
            struct bfmc_hb_motion_t hb;

            if (bfmc_hb_motion_unpack(&hb, data, dlc) != 0)
            {
                g_bfmc_app.decode_errors++;
                break;
            }

            if (s_hb_inbox[BFMC_NODE_MOTION].pending != 0U)
            {
                g_bfmc_app.hb_overruns++;
            }
            s_hb_inbox[BFMC_NODE_MOTION].counter = hb.motion_hb_counter;
            s_hb_inbox[BFMC_NODE_MOTION].rx_ms   = now;
            s_hb_inbox[BFMC_NODE_MOTION].pending = 1U;
            break;
        }

        default:
            /* MOTION_STATUS and anything else is telemetry this build does not
               act on. The accept-all filter lets it through so a capture shows
               the whole bus. */
            break;
    }
}

/* --- Gateway role -------------------------------------------------------- */

/**
 * @brief A slow triangular speed sweep, so a bus capture shows live signals.
 *
 * Placeholder for the Raspberry Pi link that arrives in Phase 4. The point
 * here is that the values move, not what they are.
 */
static int16_t BfmcApp_DemoSpeed(uint32_t now_ms)
{
    uint32_t phase = (now_ms / 10U) % 400U;  /* 0..399 over 4 s */

    return (phase < 200U) ? (int16_t)(phase * 5U)
                          : (int16_t)((400U - phase) * 5U);
}

static void BfmcApp_GatewayTask(uint32_t now)
{
    uint8_t buf[8];

    if ((uint32_t)(now - s_last_cmd_tx_ms) >= APP_MOTION_CMD_PERIOD_MS)
    {
        struct bfmc_motion_cmd_t cmd;

        s_last_cmd_tx_ms = now;

        /* The counter must advance every cycle. A consumer that sees it frozen
           declares the command stale even though frames keep arriving. */
        g_bfmc_app.gw_cmd_counter++;
        g_bfmc_app.gw_target_speed = BfmcApp_DemoSpeed(now);
        g_bfmc_app.gw_target_steer = 0;

        cmd.cmd_counter        = g_bfmc_app.gw_cmd_counter;
        cmd.target_speed       = g_bfmc_app.gw_target_speed;
        cmd.target_steer_angle = g_bfmc_app.gw_target_steer;

        if (bfmc_motion_cmd_pack(buf, &cmd, sizeof(buf)) == BFMC_MOTION_CMD_LENGTH)
        {
            (void)CanIf_Send(GATEWAY_PORT, BFMC_MOTION_CMD_FRAME_ID,
                             buf, BFMC_MOTION_CMD_LENGTH);
        }
    }

    g_bfmc_app.gw_state = Heartbeat_AggregateState(&s_gw_peers);
}

/* --- Motion role --------------------------------------------------------- */

static void BfmcApp_MotionTask(uint32_t now)
{
    uint8_t buf[8];
    uint8_t  counter;
    int16_t  speed;
    int16_t  steer;
    uint32_t rx_ms;
    uint8_t  pending;
    uint32_t dt;

    /* Drain the command inbox atomically: see the note above the inbox types.
       PRIMASK is saved and restored rather than blindly re-enabled, so this
       stays correct if it is ever called with interrupts already masked. */
    {
        uint32_t state = CriticalSection_Enter();
        pending = s_cmd_inbox.pending;
        counter = s_cmd_inbox.counter;
        speed   = s_cmd_inbox.speed;
        steer   = s_cmd_inbox.steer;
        rx_ms   = s_cmd_inbox.rx_ms;
        s_cmd_inbox.pending = 0U;
        CriticalSection_Exit(state);
    }

    if (pending != 0U)
    {
        CmdMonitor_OnFrame(&s_cmd_monitor, counter, rx_ms);
        g_bfmc_app.cmd_rx_count++;
        g_bfmc_app.mo_req_speed = speed;
        g_bfmc_app.mo_req_steer = steer;
    }

    if ((uint32_t)(now - s_last_control_ms) < APP_MOTION_CMD_PERIOD_MS)
    {
        return;
    }
    dt = (uint32_t)(now - s_last_control_ms);
    s_last_control_ms = now;

    /* Supervise, then let the fail-safe layer decide what actually reaches the
       actuators. The requested values are passed in regardless; motion_safety
       ignores them unless the command stream is healthy. */
    g_bfmc_app.mo_cmd_state = CmdMonitor_Poll(&s_cmd_monitor, now);

    MotionSafety_Update(&s_motion_out, g_bfmc_app.mo_cmd_state,
                        g_bfmc_app.mo_req_speed,
                        g_bfmc_app.mo_req_steer,
                        dt);

    g_bfmc_app.mo_out_speed = s_motion_out.speed_mm_s;
    g_bfmc_app.mo_out_steer = s_motion_out.steer_deg_x10;
    g_bfmc_app.mo_failsafe  = s_motion_out.failsafe_active ? 1U : 0U;
    g_bfmc_app.mo_state     = Heartbeat_AggregateState(&s_mo_peers);

    if ((uint32_t)(now - s_last_status_tx_ms) >= APP_MOTION_CMD_PERIOD_MS)
    {
        struct bfmc_motion_status_t st;

        s_last_status_tx_ms = now;

        memset(&st, 0, sizeof(st));
        st.meas_speed       = s_motion_out.speed_mm_s;
        st.meas_steer_angle = s_motion_out.steer_deg_x10;
        st.motor_current    = 0;  /* no sensor on this build */

        /* Bit 1 CMD_TIMEOUT, bit 2 CMD_STALE, per the MotionFlags comment in
           dbc/bfmc.dbc. */
        st.motion_flags =
            (uint16_t)(((g_bfmc_app.mo_cmd_state == CMD_MONITOR_TIMEOUT) ? 0x0002U : 0U) |
                       ((g_bfmc_app.mo_cmd_state == CMD_MONITOR_STALE)   ? 0x0004U : 0U));

        if (bfmc_motion_status_pack(buf, &st, sizeof(buf)) == BFMC_MOTION_STATUS_LENGTH)
        {
            (void)CanIf_Send(MOTION_PORT, BFMC_MOTION_STATUS_FRAME_ID,
                             buf, BFMC_MOTION_STATUS_LENGTH);
        }
    }
}

/* --- Heartbeats ---------------------------------------------------------- */

static void BfmcApp_DrainHeartbeats(uint32_t now)
{
    uint8_t i;

    for (i = 0U; i < HEARTBEAT_MAX_NODES; i++)
    {
        uint8_t  pending;
        uint8_t  counter;
        uint32_t rx_ms;

        {
            uint32_t state = CriticalSection_Enter();
            pending = s_hb_inbox[i].pending;
            counter = s_hb_inbox[i].counter;
            rx_ms   = s_hb_inbox[i].rx_ms;
            s_hb_inbox[i].pending = 0U;
            CriticalSection_Exit(state);
        }

        if (pending == 0U)
        {
            continue;
        }

        /* Feed both supervisors: each ignores nodes it did not register. */
        Heartbeat_OnFrame(&s_gw_peers, i, counter, rx_ms);
        Heartbeat_OnFrame(&s_mo_peers, i, counter, rx_ms);
    }

    (void)Heartbeat_Poll(&s_gw_peers, now);
    (void)Heartbeat_Poll(&s_mo_peers, now);
}

static void BfmcApp_HeartbeatTxTask(uint32_t now)
{
    uint8_t buf[8];

    if ((uint32_t)(now - s_last_hb_tx_ms) < APP_HEARTBEAT_PERIOD_MS)
    {
        return;
    }
    s_last_hb_tx_ms = now;
    s_hb_counter++;

    {
        struct bfmc_hb_gateway_t hb;

        hb.gateway_hb_counter = s_hb_counter;
        hb.gateway_node_state = (uint8_t)g_bfmc_app.gw_state;

        if (bfmc_hb_gateway_pack(buf, &hb, sizeof(buf)) == BFMC_HB_GATEWAY_LENGTH)
        {
            (void)CanIf_Send(GATEWAY_PORT, BFMC_HB_GATEWAY_FRAME_ID,
                             buf, BFMC_HB_GATEWAY_LENGTH);
        }
    }

    {
        struct bfmc_hb_motion_t hb;

        hb.motion_hb_counter = s_hb_counter;
        hb.motion_node_state = (uint8_t)g_bfmc_app.mo_state;

        if (bfmc_hb_motion_pack(buf, &hb, sizeof(buf)) == BFMC_HB_MOTION_LENGTH)
        {
            (void)CanIf_Send(MOTION_PORT, BFMC_HB_MOTION_FRAME_ID,
                             buf, BFMC_HB_MOTION_LENGTH);
        }
    }
}

/* --- Entry points -------------------------------------------------------- */

void BfmcApp_Init(void)
{
    uint32_t now;

    memset((void *)&g_bfmc_app, 0, sizeof(g_bfmc_app));
    memset(&s_cmd_inbox, 0, sizeof(s_cmd_inbox));
    memset(s_hb_inbox, 0, sizeof(s_hb_inbox));

    CmdMonitor_Init(&s_cmd_monitor,
                    APP_MOTION_CMD_TIMEOUT_MS,
                    APP_MOTION_CMD_STALE_MS);

    MotionSafety_Init(&s_motion_out, APP_FAILSAFE_DECEL_MM_S2);

    /* Each role registers only the peers that exist in this build. The Sensor
       ECU is deliberately not registered: registering a node that was never
       built would hold the vehicle in DEGRADED forever and say nothing useful.
       It joins when that node does. */
    Heartbeat_Init(&s_gw_peers, APP_HEARTBEAT_TIMEOUT_MS);
    Heartbeat_Register(&s_gw_peers, BFMC_NODE_MOTION, true);

    Heartbeat_Init(&s_mo_peers, APP_HEARTBEAT_TIMEOUT_MS);
    Heartbeat_Register(&s_mo_peers, BFMC_NODE_GATEWAY, true);

    CanIf_Init(BfmcApp_OnFrame);

    now = HAL_GetTick();
    s_last_cmd_tx_ms    = now;
    s_last_status_tx_ms = now;
    s_last_hb_tx_ms     = now;
    s_last_control_ms   = now;
}

void BfmcApp_Task(void)
{
    uint32_t now = HAL_GetTick();

    /* Refresh error state and drive bus-off recovery before anything else, so
       the rest of the cycle acts on a current view of the transport. */
    CanIf_Poll(now);
    g_bfmc_app.can_health = CanIf_WorstHealth();

    BfmcApp_DrainHeartbeats(now);
    BfmcApp_GatewayTask(now);
    BfmcApp_MotionTask(now);
    BfmcApp_HeartbeatTxTask(now);
}
