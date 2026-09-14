/**
 * Host-side tests for the application layer.
 *
 * These are the safety behaviours: command timeout, frozen-counter detection,
 * heartbeat supervision and the fail-safe actuator ramp. All of them are
 * defined in terms of elapsed time, so on real hardware they are slow and
 * awkward to provoke -- you would have to physically unplug a node and catch
 * the reaction. Because every module takes the clock as a parameter, a test can
 * drive a synthetic clock and assert the exact millisecond of each transition.
 *
 * Builds with plain gcc. No HAL, no hardware.
 */
#include "test_util.h"

#include "app_config.h"
#include "cmd_monitor.h"
#include "heartbeat.h"
#include "motion_safety.h"
#include "node_state.h"

#include <stdint.h>

/* Node ids mirror the NodeAddress attribute in dbc/bfmc.dbc. */
#define NODE_GATEWAY 1U
#define NODE_MOTION  2U
#define NODE_SENSOR  3U
#define NODE_LOGGER  4U

/* ================================================================== */
/* Command supervision                                                 */
/* ================================================================== */

static void test_cmd_monitor_starts_unusable(void)
{
    cmd_monitor_t mon;

    CmdMonitor_Init(&mon, APP_MOTION_CMD_TIMEOUT_MS, APP_MOTION_CMD_STALE_MS);

    /* Before the first command the vehicle must not be drivable, and this is
       reported as INIT rather than TIMEOUT: startup is not a fault. */
    CHECK_EQ(CmdMonitor_Poll(&mon, 0), CMD_MONITOR_INIT);
    CHECK(!CmdMonitor_CommandIsUsable(&mon));

    CHECK_EQ(CmdMonitor_Poll(&mon, 10000), CMD_MONITOR_INIT);
    CHECK(!CmdMonitor_CommandIsUsable(&mon));
}

static void test_cmd_monitor_ok_while_fresh(void)
{
    cmd_monitor_t mon;
    uint32_t t;
    uint8_t counter = 0;

    CmdMonitor_Init(&mon, APP_MOTION_CMD_TIMEOUT_MS, APP_MOTION_CMD_STALE_MS);

    for (t = 0; t <= 1000; t += APP_MOTION_CMD_PERIOD_MS) {
        CmdMonitor_OnFrame(&mon, counter++, t);
        CHECK_EQ(CmdMonitor_Poll(&mon, t), CMD_MONITOR_OK);
    }
    CHECK(CmdMonitor_CommandIsUsable(&mon));
}

static void test_cmd_monitor_timeout_boundary(void)
{
    cmd_monitor_t mon;

    CmdMonitor_Init(&mon, APP_MOTION_CMD_TIMEOUT_MS, APP_MOTION_CMD_STALE_MS);
    CmdMonitor_OnFrame(&mon, 1, 0);

    /* One millisecond inside the budget is still healthy... */
    CHECK_EQ(CmdMonitor_Poll(&mon, APP_MOTION_CMD_TIMEOUT_MS - 1), CMD_MONITOR_OK);
    CHECK(CmdMonitor_CommandIsUsable(&mon));

    /* ...and the budget itself trips it. Asserting both sides pins the
       boundary, so an off-by-one in the comparison cannot pass. */
    CHECK_EQ(CmdMonitor_Poll(&mon, APP_MOTION_CMD_TIMEOUT_MS), CMD_MONITOR_TIMEOUT);
    CHECK(!CmdMonitor_CommandIsUsable(&mon));
}

static void test_cmd_monitor_detects_frozen_counter(void)
{
    cmd_monitor_t mon;
    uint32_t t;

    CmdMonitor_Init(&mon, APP_MOTION_CMD_TIMEOUT_MS, APP_MOTION_CMD_STALE_MS);

    /* A wedged Gateway: frames keep arriving exactly on schedule, so a plain
       timeout never fires, but the payload never changes. */
    CmdMonitor_OnFrame(&mon, 42, 0);

    for (t = APP_MOTION_CMD_PERIOD_MS; t < APP_MOTION_CMD_STALE_MS;
         t += APP_MOTION_CMD_PERIOD_MS) {
        CmdMonitor_OnFrame(&mon, 42, t);
        CHECK_EQ(CmdMonitor_Poll(&mon, t), CMD_MONITOR_OK);
    }

    CmdMonitor_OnFrame(&mon, 42, APP_MOTION_CMD_STALE_MS);
    CHECK_EQ(CmdMonitor_Poll(&mon, APP_MOTION_CMD_STALE_MS), CMD_MONITOR_STALE);
    CHECK(!CmdMonitor_CommandIsUsable(&mon));
}

static void test_cmd_monitor_recovers(void)
{
    cmd_monitor_t mon;

    CmdMonitor_Init(&mon, APP_MOTION_CMD_TIMEOUT_MS, APP_MOTION_CMD_STALE_MS);
    CmdMonitor_OnFrame(&mon, 1, 0);

    CHECK_EQ(CmdMonitor_Poll(&mon, 500), CMD_MONITOR_TIMEOUT);

    /* A fault must not latch here. Latching is the right policy for bus-off,
       but a momentarily lost command should recover as soon as the stream
       resumes; otherwise a single dropout parks the vehicle permanently. */
    CmdMonitor_OnFrame(&mon, 2, 500);
    CHECK_EQ(CmdMonitor_Poll(&mon, 500), CMD_MONITOR_OK);
}

static void test_cmd_monitor_counter_wraps(void)
{
    cmd_monitor_t mon;

    CmdMonitor_Init(&mon, APP_MOTION_CMD_TIMEOUT_MS, APP_MOTION_CMD_STALE_MS);

    /* CmdCounter is uint8: 255 -> 0 is ordinary progress, not a frozen sender. */
    CmdMonitor_OnFrame(&mon, 255, 0);
    CmdMonitor_OnFrame(&mon, 0, 20);
    CmdMonitor_OnFrame(&mon, 1, 40);
    CHECK_EQ(CmdMonitor_Poll(&mon, 60), CMD_MONITOR_OK);
}

static void test_cmd_monitor_survives_tick_wraparound(void)
{
    cmd_monitor_t mon;
    const uint32_t near_wrap = 0xFFFFFFF0UL;

    CmdMonitor_Init(&mon, APP_MOTION_CMD_TIMEOUT_MS, APP_MOTION_CMD_STALE_MS);

    /* HAL_GetTick() wraps after ~49.7 days. A signed or naive comparison would
       read the wrap as a huge elapsed time and trip fail-safe on a vehicle that
       is running perfectly. */
    CmdMonitor_OnFrame(&mon, 1, near_wrap);

    CHECK_EQ(CmdMonitor_Poll(&mon, near_wrap + APP_MOTION_CMD_TIMEOUT_MS - 1),
             CMD_MONITOR_OK);
    CHECK_EQ(CmdMonitor_Poll(&mon, near_wrap + APP_MOTION_CMD_TIMEOUT_MS),
             CMD_MONITOR_TIMEOUT);
}

/* ================================================================== */
/* Heartbeat supervision                                               */
/* ================================================================== */

static void heartbeat_setup(heartbeat_monitor_t *mon)
{
    Heartbeat_Init(mon, APP_HEARTBEAT_TIMEOUT_MS);
    Heartbeat_Register(mon, NODE_GATEWAY, true);
    Heartbeat_Register(mon, NODE_SENSOR, true);
    Heartbeat_Register(mon, NODE_LOGGER, false); /* passive, never required */
}

static void test_heartbeat_absent_before_first_frame(void)
{
    heartbeat_monitor_t mon;

    heartbeat_setup(&mon);

    CHECK(Heartbeat_Poll(&mon, 0) != 0U);
    CHECK_EQ(Heartbeat_Status(&mon, NODE_SENSOR), HEARTBEAT_ABSENT);
    CHECK(!Heartbeat_AllRequiredAlive(&mon));
    CHECK_EQ(Heartbeat_AggregateState(&mon), NODE_STATE_DEGRADED);
}

static void test_heartbeat_running_when_all_alive(void)
{
    heartbeat_monitor_t mon;
    uint8_t counter = 0;
    uint32_t t;

    heartbeat_setup(&mon);

    for (t = 0; t <= 1000; t += APP_HEARTBEAT_PERIOD_MS) {
        Heartbeat_OnFrame(&mon, NODE_GATEWAY, counter, t);
        Heartbeat_OnFrame(&mon, NODE_SENSOR, counter, t);
        counter++;

        CHECK_EQ(Heartbeat_Poll(&mon, t), 0U);
    }

    CHECK(Heartbeat_AllRequiredAlive(&mon));
    CHECK_EQ(Heartbeat_AggregateState(&mon), NODE_STATE_RUNNING);
}

static void test_heartbeat_lost_sensor_degrades(void)
{
    heartbeat_monitor_t mon;
    uint8_t counter = 0;
    uint32_t t;
    uint32_t mask;

    heartbeat_setup(&mon);

    for (t = 0; t <= 300; t += APP_HEARTBEAT_PERIOD_MS) {
        Heartbeat_OnFrame(&mon, NODE_GATEWAY, counter, t);
        Heartbeat_OnFrame(&mon, NODE_SENSOR, counter, t);
        counter++;
    }
    CHECK_EQ(Heartbeat_Poll(&mon, 300), 0U);

    /* The Sensor ECU dies at t=300; the Gateway keeps publishing. */
    for (t = 400; t < 600; t += APP_HEARTBEAT_PERIOD_MS) {
        Heartbeat_OnFrame(&mon, NODE_GATEWAY, counter++, t);
    }

    CHECK_EQ(Heartbeat_Poll(&mon, 599), 0U); /* still inside the budget */

    mask = Heartbeat_Poll(&mon, 600);
    CHECK_EQ(mask, 1UL << NODE_SENSOR);
    CHECK_EQ(Heartbeat_Status(&mon, NODE_SENSOR), HEARTBEAT_LOST);
    CHECK_EQ(Heartbeat_Status(&mon, NODE_GATEWAY), HEARTBEAT_ALIVE);
    CHECK_EQ(Heartbeat_AggregateState(&mon), NODE_STATE_DEGRADED);
}

static void test_heartbeat_frozen_counter_is_stale(void)
{
    heartbeat_monitor_t mon;
    uint32_t t;

    heartbeat_setup(&mon);

    /* Both nodes transmit forever, but the Sensor counter never moves. */
    for (t = 0; t <= APP_HEARTBEAT_TIMEOUT_MS; t += APP_HEARTBEAT_PERIOD_MS) {
        Heartbeat_OnFrame(&mon, NODE_GATEWAY, (uint8_t)(t / APP_HEARTBEAT_PERIOD_MS), t);
        Heartbeat_OnFrame(&mon, NODE_SENSOR, 7, t);
    }

    CHECK_EQ(Heartbeat_Poll(&mon, APP_HEARTBEAT_TIMEOUT_MS), 1UL << NODE_SENSOR);
    CHECK_EQ(Heartbeat_Status(&mon, NODE_SENSOR), HEARTBEAT_STALE);
    CHECK_EQ(Heartbeat_AggregateState(&mon), NODE_STATE_DEGRADED);
}

static void test_heartbeat_optional_node_is_not_a_fault(void)
{
    heartbeat_monitor_t mon;
    uint32_t t;
    uint8_t counter = 0;

    heartbeat_setup(&mon);

    /* The Logger is passive. Nobody depends on it, so its absence must not
       degrade the vehicle. */
    for (t = 0; t <= 1000; t += APP_HEARTBEAT_PERIOD_MS) {
        Heartbeat_OnFrame(&mon, NODE_GATEWAY, counter, t);
        Heartbeat_OnFrame(&mon, NODE_SENSOR, counter, t);
        counter++;
    }

    CHECK_EQ(Heartbeat_Poll(&mon, 1000), 0U);
    CHECK_EQ(Heartbeat_Status(&mon, NODE_LOGGER), HEARTBEAT_ABSENT);
    CHECK_EQ(Heartbeat_AggregateState(&mon), NODE_STATE_RUNNING);
}

static void test_heartbeat_ignores_unregistered_nodes(void)
{
    heartbeat_monitor_t mon;

    heartbeat_setup(&mon);

    /* An unexpected node appearing on the bus must not alter our state. */
    Heartbeat_OnFrame(&mon, NODE_MOTION, 1, 0);
    CHECK_EQ(Heartbeat_Status(&mon, NODE_MOTION), HEARTBEAT_ABSENT);

    /* Out-of-range ids must be rejected rather than corrupting memory. */
    Heartbeat_OnFrame(&mon, HEARTBEAT_MAX_NODES, 1, 0);
    Heartbeat_Register(&mon, HEARTBEAT_MAX_NODES + 3U, true);
    CHECK_EQ(Heartbeat_Status(&mon, HEARTBEAT_MAX_NODES), HEARTBEAT_ABSENT);
}

/* ================================================================== */
/* Fail-safe actuator output                                           */
/* ================================================================== */

static void test_motion_safety_passes_through_when_ok(void)
{
    motion_safety_t ms;

    MotionSafety_Init(&ms, APP_FAILSAFE_DECEL_MM_S2);
    MotionSafety_Update(&ms, CMD_MONITOR_OK, 1200, -150, APP_MOTION_CMD_PERIOD_MS);

    CHECK_EQ(ms.speed_mm_s, 1200);
    CHECK_EQ(ms.steer_deg_x10, -150);
    CHECK(!ms.failsafe_active);
}

static void test_motion_safety_ramps_to_stop_and_holds_steering(void)
{
    motion_safety_t ms;
    int cycles = 0;
    int16_t previous;

    MotionSafety_Init(&ms, APP_FAILSAFE_DECEL_MM_S2);
    MotionSafety_Update(&ms, CMD_MONITOR_OK, 1500, 200, APP_MOTION_CMD_PERIOD_MS);

    previous = ms.speed_mm_s;
    while (!MotionSafety_IsStopped(&ms) && cycles < 1000) {
        MotionSafety_Update(&ms, CMD_MONITOR_TIMEOUT, 1500, 200,
                            APP_MOTION_CMD_PERIOD_MS);
        cycles++;

        CHECK(ms.speed_mm_s < previous);  /* strictly monotonic */
        CHECK(ms.speed_mm_s >= 0);        /* never overshoots into reverse */
        previous = ms.speed_mm_s;
    }

    CHECK(MotionSafety_IsStopped(&ms));
    CHECK(ms.failsafe_active);

    /* 1500 mm/s at 4000 mm/s^2 is 375 ms, so 19 cycles of 20 ms. Assert the
       vehicle stops well inside half a second rather than pinning the exact
       count, which would break on any ramp-rate tuning. */
    CHECK(cycles * (int)APP_MOTION_CMD_PERIOD_MS <= 500);

    /* Steering holds its last commanded value throughout. */
    CHECK_EQ(ms.steer_deg_x10, 200);
}

static void test_motion_safety_ramps_up_from_reverse(void)
{
    motion_safety_t ms;
    int cycles = 0;

    MotionSafety_Init(&ms, APP_FAILSAFE_DECEL_MM_S2);
    MotionSafety_Update(&ms, CMD_MONITOR_OK, -800, 0, APP_MOTION_CMD_PERIOD_MS);

    while (!MotionSafety_IsStopped(&ms) && cycles < 1000) {
        MotionSafety_Update(&ms, CMD_MONITOR_STALE, -800, 0,
                            APP_MOTION_CMD_PERIOD_MS);
        cycles++;
        CHECK(ms.speed_mm_s <= 0); /* approaches zero from below, no overshoot */
    }

    CHECK(MotionSafety_IsStopped(&ms));
}

static void test_motion_safety_always_makes_progress(void)
{
    motion_safety_t ms;
    int cycles = 0;

    /* A gentle ramp and a 1 ms cycle make the integer step round to zero.
       Without a minimum step the vehicle would coast at its last speed
       forever while believing it was braking. */
    MotionSafety_Init(&ms, 100);
    MotionSafety_Update(&ms, CMD_MONITOR_OK, 50, 0, 1);

    while (!MotionSafety_IsStopped(&ms) && cycles < 10000) {
        MotionSafety_Update(&ms, CMD_MONITOR_TIMEOUT, 50, 0, 1);
        cycles++;
    }

    CHECK(MotionSafety_IsStopped(&ms));
}

static void test_motion_safety_init_state_does_not_drive(void)
{
    motion_safety_t ms;

    MotionSafety_Init(&ms, APP_FAILSAFE_DECEL_MM_S2);

    /* Before the first valid command, output must be standstill even though a
       request value is present. */
    MotionSafety_Update(&ms, CMD_MONITOR_INIT, 2000, 100,
                        APP_MOTION_CMD_PERIOD_MS);

    CHECK_EQ(ms.speed_mm_s, 0);
    CHECK(ms.failsafe_active);
}

/* ================================================================== */
/* Wire contract                                                       */
/* ================================================================== */

static void test_node_state_values_match_dbc(void)
{
    /* These are transmitted in the NodeState signal, so they must match the
       VAL_ tables in dbc/bfmc.dbc. tools/validate_dbc.py checks the database
       side of the same contract. */
    CHECK_EQ(NODE_STATE_INIT,     0);
    CHECK_EQ(NODE_STATE_RUNNING,  1);
    CHECK_EQ(NODE_STATE_DEGRADED, 2);
    CHECK_EQ(NODE_STATE_BUS_OFF,  3);
    CHECK_EQ(NODE_STATE_FAULT,    4);
}

static void test_timing_budgets_are_three_cycles(void)
{
    /* The "three missed cycles" rule from docs/message-matrix.md. */
    CHECK_EQ(APP_MOTION_CMD_TIMEOUT_MS, 3 * APP_MOTION_CMD_PERIOD_MS);
    CHECK_EQ(APP_HEARTBEAT_TIMEOUT_MS, 3 * APP_HEARTBEAT_PERIOD_MS);
    CHECK_EQ(APP_MOTION_CMD_TIMEOUT_MS, 60);
    CHECK_EQ(APP_HEARTBEAT_TIMEOUT_MS, 300);
}

int main(void)
{
    test_cmd_monitor_starts_unusable();
    test_cmd_monitor_ok_while_fresh();
    test_cmd_monitor_timeout_boundary();
    test_cmd_monitor_detects_frozen_counter();
    test_cmd_monitor_recovers();
    test_cmd_monitor_counter_wraps();
    test_cmd_monitor_survives_tick_wraparound();

    test_heartbeat_absent_before_first_frame();
    test_heartbeat_running_when_all_alive();
    test_heartbeat_lost_sensor_degrades();
    test_heartbeat_frozen_counter_is_stale();
    test_heartbeat_optional_node_is_not_a_fault();
    test_heartbeat_ignores_unregistered_nodes();

    test_motion_safety_passes_through_when_ok();
    test_motion_safety_ramps_to_stop_and_holds_steering();
    test_motion_safety_ramps_up_from_reverse();
    test_motion_safety_always_makes_progress();
    test_motion_safety_init_state_does_not_drive();

    test_node_state_values_match_dbc();
    test_timing_budgets_are_three_cycles();

    return test_report();
}
