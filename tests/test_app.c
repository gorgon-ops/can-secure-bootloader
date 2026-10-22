#include "test_util.h"

#include "app_config.h"
#include "cmd_monitor.h"
#include "heartbeat.h"
#include "motion_safety.h"
#include "node_state.h"
#include "can_filter_plan.h"
#include "task_watchdog.h"

#include "bfmc.h"

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

    /* INIT, not TIMEOUT: startup is not a fault. */
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

    CHECK_EQ(CmdMonitor_Poll(&mon, APP_MOTION_CMD_TIMEOUT_MS - 1), CMD_MONITOR_OK);
    CHECK(CmdMonitor_CommandIsUsable(&mon));

    /* Both sides of the boundary, so an off-by-one cannot pass. */
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

    /* Must not latch: a momentary dropout recovers when the stream resumes. */
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

    /* HAL_GetTick() wraps after ~49.7 days. */
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

    /* Nobody depends on the passive Logger. */
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

    /* 1500 mm/s at 4000 mm/s^2 is 375 ms. Bounded rather than pinned, so ramp
     * tuning does not break it. */
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

    /* A gentle ramp at 1 ms rounds the integer step to zero. */
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
/* Task watchdog                                                       */
/* ================================================================== */

#define WDT_TASK_A 0U
#define WDT_TASK_B 1U
#define WDT_GRACE_MS 1000U

static void watchdog_setup(task_watchdog_t *wdt)
{
    TaskWatchdog_Init(wdt, 0, WDT_GRACE_MS);
    TaskWatchdog_Register(wdt, WDT_TASK_A, 100);
    TaskWatchdog_Register(wdt, WDT_TASK_B, 500);
}

static void test_watchdog_grace_allows_boot(void)
{
    task_watchdog_t wdt;

    watchdog_setup(&wdt);

    /* Grace window, or the board resets on every boot. */
    CHECK(TaskWatchdog_MayRefresh(&wdt, 0));
    CHECK(TaskWatchdog_MayRefresh(&wdt, WDT_GRACE_MS - 1));
}

static void test_watchdog_task_that_never_starts_is_a_fault(void)
{
    task_watchdog_t wdt;

    watchdog_setup(&wdt);

    /* A task that never runs is as dangerous as one that stops. */
    CHECK(!TaskWatchdog_MayRefresh(&wdt, WDT_GRACE_MS));
    CHECK_EQ(TaskWatchdog_OverdueMask(&wdt),
             (1UL << WDT_TASK_A) | (1UL << WDT_TASK_B));
}

static void test_watchdog_healthy_tasks_keep_refreshing(void)
{
    task_watchdog_t wdt;
    uint32_t t;

    watchdog_setup(&wdt);

    for (t = 0; t <= 5000; t += 20) {
        TaskWatchdog_Checkin(&wdt, WDT_TASK_A, t);
        if ((t % 100) == 0) {
            TaskWatchdog_Checkin(&wdt, WDT_TASK_B, t);
        }
        CHECK(TaskWatchdog_MayRefresh(&wdt, t));
    }
    CHECK_EQ(TaskWatchdog_OverdueMask(&wdt), 0U);
}

static void test_watchdog_catches_one_stalled_task(void)
{
    task_watchdog_t wdt;
    uint32_t t;

    watchdog_setup(&wdt);

    for (t = 0; t <= 2000; t += 20) {
        TaskWatchdog_Checkin(&wdt, WDT_TASK_A, t);
        TaskWatchdog_Checkin(&wdt, WDT_TASK_B, t);
    }
    CHECK(TaskWatchdog_MayRefresh(&wdt, 2000));

    /* B keeps checking in, so a watchdog kicked from the loop would not notice. */
    for (t = 2020; t < 2100; t += 20) {
        TaskWatchdog_Checkin(&wdt, WDT_TASK_B, t);
        CHECK(TaskWatchdog_MayRefresh(&wdt, t));
    }

    TaskWatchdog_Checkin(&wdt, WDT_TASK_B, 2100);
    CHECK(!TaskWatchdog_MayRefresh(&wdt, 2100));
    CHECK_EQ(TaskWatchdog_OverdueMask(&wdt), 1UL << WDT_TASK_A);
}

static void test_watchdog_is_latched(void)
{
    task_watchdog_t wdt;

    watchdog_setup(&wdt);
    TaskWatchdog_Checkin(&wdt, WDT_TASK_A, 0);
    TaskWatchdog_Checkin(&wdt, WDT_TASK_B, 0);

    CHECK(!TaskWatchdog_MayRefresh(&wdt, 500));

    /* Checking in late must not rescue it. */
    TaskWatchdog_Checkin(&wdt, WDT_TASK_A, 500);
    TaskWatchdog_Checkin(&wdt, WDT_TASK_B, 500);
    CHECK(!TaskWatchdog_MayRefresh(&wdt, 500));
    CHECK(!TaskWatchdog_MayRefresh(&wdt, 10000));
}

static void test_watchdog_deadline_boundary(void)
{
    task_watchdog_t wdt;

    watchdog_setup(&wdt);
    TaskWatchdog_Checkin(&wdt, WDT_TASK_A, 1000);
    TaskWatchdog_Checkin(&wdt, WDT_TASK_B, 1000);

    CHECK(TaskWatchdog_MayRefresh(&wdt, 1099));   /* 99 ms: inside  */
    CHECK(!TaskWatchdog_MayRefresh(&wdt, 1100));  /* 100 ms: expired */
}

static void test_watchdog_survives_tick_wraparound(void)
{
    task_watchdog_t wdt;
    const uint32_t near_wrap = 0xFFFFFFF0UL;

    TaskWatchdog_Init(&wdt, near_wrap, WDT_GRACE_MS);
    TaskWatchdog_Register(&wdt, WDT_TASK_A, 100);
    TaskWatchdog_Checkin(&wdt, WDT_TASK_A, near_wrap);

    CHECK(TaskWatchdog_MayRefresh(&wdt, near_wrap + 99));
    CHECK(!TaskWatchdog_MayRefresh(&wdt, near_wrap + 100));
}

static void test_watchdog_ignores_out_of_range_ids(void)
{
    task_watchdog_t wdt;

    watchdog_setup(&wdt);
    TaskWatchdog_Register(&wdt, TASK_WATCHDOG_MAX_TASKS, 100);
    TaskWatchdog_Checkin(&wdt, TASK_WATCHDOG_MAX_TASKS + 5U, 0);

    TaskWatchdog_Checkin(&wdt, WDT_TASK_A, 0);
    TaskWatchdog_Checkin(&wdt, WDT_TASK_B, 0);
    CHECK(TaskWatchdog_MayRefresh(&wdt, 50));
}

/* ================================================================== */
/* Receive filter plans                                                */
/* ================================================================== */

/* The IDs BfmcApp_OnFrame() dispatches on, restated independently on purpose:
 * a filter that drops one fails silently. */
static const uint16_t k_dispatched_ids[] = {
    BFMC_MOTION_CMD_FRAME_ID,
    BFMC_HB_GATEWAY_FRAME_ID,
    BFMC_HB_MOTION_FRAME_ID,
};
#define DISPATCHED_COUNT (sizeof(k_dispatched_ids) / sizeof(k_dispatched_ids[0]))

static void test_filter_loopback_covers_everything_dispatched(void)
{
    can_filter_plan_t plan;
    size_t i;

    /* Loopback: CAN1 must accept the union, including the Gateway's own
     * MOTION_CMD. */
    CanFilterPlan_Build(&plan, BFMC_ROLE_GATEWAY | BFMC_ROLE_MOTION);

    for (i = 0; i < DISPATCHED_COUNT; i++) {
        CHECK(CanFilterPlan_Accepts(&plan, k_dispatched_ids[i]));
    }
}

static void test_filter_dual_covers_everything_dispatched(void)
{
    can_filter_plan_t gateway;
    can_filter_plan_t motion;

    CanFilterPlan_Build(&gateway, BFMC_ROLE_GATEWAY);
    CanFilterPlan_Build(&motion, BFMC_ROLE_MOTION);

    /* Dual: the Gateway transmits MOTION_CMD and HB_GATEWAY on CAN1, so the
       Motion port must accept both. */
    CHECK(CanFilterPlan_Accepts(&motion, BFMC_MOTION_CMD_FRAME_ID));
    CHECK(CanFilterPlan_Accepts(&motion, BFMC_HB_GATEWAY_FRAME_ID));

    /* And Motion transmits HB_MOTION and MOTION_STATUS back. */
    CHECK(CanFilterPlan_Accepts(&gateway, BFMC_HB_MOTION_FRAME_ID));
    CHECK(CanFilterPlan_Accepts(&gateway, BFMC_MOTION_STATUS_FRAME_ID));
}

static void test_filter_excludes_what_a_role_does_not_use(void)
{
    can_filter_plan_t gateway;
    can_filter_plan_t motion;

    CanFilterPlan_Build(&gateway, BFMC_ROLE_GATEWAY);
    CanFilterPlan_Build(&motion, BFMC_ROLE_MOTION);

    /* If these ever pass, the filter has silently widened back to accept-all. */
    CHECK(!CanFilterPlan_Accepts(&gateway, BFMC_MOTION_CMD_FRAME_ID));
    CHECK(!CanFilterPlan_Accepts(&gateway, BFMC_HB_GATEWAY_FRAME_ID));
    CHECK(!CanFilterPlan_Accepts(&motion, BFMC_IMU_ACCEL_FRAME_ID));
    CHECK(!CanFilterPlan_Accepts(&motion, BFMC_BATTERY_FRAME_ID));
    CHECK(!CanFilterPlan_Accepts(&motion, BFMC_MOTION_STATUS_FRAME_ID));

    /* A bank padded with zeroes would subscribe the node to 0x000. */
    CHECK(!CanFilterPlan_Accepts(&gateway, 0x000));
    CHECK(!CanFilterPlan_Accepts(&motion, 0x000));
    CHECK(!CanFilterPlan_Accepts(&gateway, 0x7FF));
}

static void test_filter_union_is_deduplicated_and_sorted(void)
{
    can_filter_plan_t gateway;
    can_filter_plan_t motion;
    can_filter_plan_t both;
    uint8_t i;
    uint8_t shared = 0;

    CanFilterPlan_Build(&gateway, BFMC_ROLE_GATEWAY);
    CanFilterPlan_Build(&motion, BFMC_ROLE_MOTION);
    CanFilterPlan_Build(&both, BFMC_ROLE_GATEWAY | BFMC_ROLE_MOTION);

    /* EMERGENCY_STOP and HB_SENSOR are in both role lists. */
    for (i = 0; i < gateway.count; i++) {
        if (CanFilterPlan_Accepts(&motion, gateway.ids[i])) {
            shared++;
        }
    }
    CHECK(shared > 0);
    CHECK_EQ(both.count, gateway.count + motion.count - shared);

    for (i = 1; i < both.count; i++) {
        CHECK(both.ids[i - 1] < both.ids[i]); /* sorted, strictly ascending */
    }
}

static void test_filter_fits_its_bank_budget(void)
{
    can_filter_plan_t both;

    CanFilterPlan_Build(&both, BFMC_ROLE_GATEWAY | BFMC_ROLE_MOTION);

    CHECK(both.count <= CAN_FILTER_PLAN_MAX_IDS);

    /* Four IDs per bank, CAN1 owns banks 0..13. */
    CHECK_EQ(CanFilterPlan_BanksRequired(&both), (both.count + 3) / 4);
    CHECK(CanFilterPlan_BanksRequired(&both) <= 14);
}

/* ================================================================== */
/* Wire contract                                                       */
/* ================================================================== */

static void test_node_state_values_match_dbc(void)
{
    /* Must match the VAL_ tables in dbc/bfmc.dbc; tools/validate_dbc.py checks the
     * database side. */
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

    test_watchdog_grace_allows_boot();
    test_watchdog_task_that_never_starts_is_a_fault();
    test_watchdog_healthy_tasks_keep_refreshing();
    test_watchdog_catches_one_stalled_task();
    test_watchdog_is_latched();
    test_watchdog_deadline_boundary();
    test_watchdog_survives_tick_wraparound();
    test_watchdog_ignores_out_of_range_ids();

    test_filter_loopback_covers_everything_dispatched();
    test_filter_dual_covers_everything_dispatched();
    test_filter_excludes_what_a_role_does_not_use();
    test_filter_union_is_deduplicated_and_sorted();
    test_filter_fits_its_bank_budget();

    test_node_state_values_match_dbc();
    test_timing_budgets_are_three_cycles();

    return test_report();
}
