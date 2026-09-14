/**
 * Host-side tests for the generated CAN protocol layer.
 *
 * Builds with plain gcc - no STM32 HAL, no hardware. Two modes:
 *
 *   (no args)         run assertions, exit non-zero on any failure
 *   --dump-vectors    print encodings as "NAME hexbytes" for cross_check.py
 *                     to compare against the independent cantools encoder
 *
 * Round-trip tests catch a generator or DBC change that breaks symmetry. The
 * cross-check catches something round-trips cannot: both directions being
 * wrong in the same way, which is what happens when a bit position or the
 * endianness of a signal is wrong in the DBC itself.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "bfmc.h"

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long _a = (long)(a), _b = (long)(b);                                 \
        g_checks++;                                                          \
        if (_a != _b) {                                                      \
            g_failures++;                                                    \
            printf("FAIL %s:%d  %s == %s  (%ld != %ld)\n",                   \
                   __FILE__, __LINE__, #a, #b, _a, _b);                      \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */

static void test_motion_cmd_roundtrip(void)
{
    /* Extremes and negatives, not only happy-path values: sign handling in a
       packed bitfield is exactly where generated and hand-written code differ. */
    const struct bfmc_motion_cmd_t cases[] = {
        {     0,    0,   0 },
        {  1500,  123,   1 },
        { -1500, -123, 255 },
        {  2000,  250, 128 },
        { -2000, -250,  42 },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t buf[8];
        struct bfmc_motion_cmd_t out;
        int packed;

        memset(buf, 0xAA, sizeof(buf));
        packed = bfmc_motion_cmd_pack(buf, &cases[i], sizeof(buf));
        CHECK_EQ(packed, BFMC_MOTION_CMD_LENGTH);

        memset(&out, 0, sizeof(out));
        CHECK_EQ(bfmc_motion_cmd_unpack(&out, buf, (size_t)packed), 0);

        CHECK_EQ(out.target_speed,       cases[i].target_speed);
        CHECK_EQ(out.target_steer_angle, cases[i].target_steer_angle);
        CHECK_EQ(out.cmd_counter,        cases[i].cmd_counter);
    }
}

static void test_motion_status_roundtrip(void)
{
    struct bfmc_motion_status_t in;
    struct bfmc_motion_status_t out;
    uint8_t buf[8];

    in.meas_speed       = -874;
    in.meas_steer_angle = 231;
    in.motor_current    = -12345;
    in.motion_flags     = 0xBEEF;

    CHECK_EQ(bfmc_motion_status_pack(buf, &in, sizeof(buf)),
             BFMC_MOTION_STATUS_LENGTH);
    CHECK_EQ(bfmc_motion_status_unpack(&out, buf, sizeof(buf)), 0);
    CHECK_EQ(out.meas_speed,       in.meas_speed);
    CHECK_EQ(out.meas_steer_angle, in.meas_steer_angle);
    CHECK_EQ(out.motor_current,    in.motor_current);
    CHECK_EQ(out.motion_flags,     in.motion_flags);
}

static void test_imu_gyro_roundtrip(void)
{
    struct bfmc_imu_gyro_t in;
    struct bfmc_imu_gyro_t out;
    uint8_t buf[8];

    in.gyro_x = -2000;
    in.gyro_y = 0;
    in.gyro_z = 1999;

    CHECK_EQ(bfmc_imu_gyro_pack(buf, &in, sizeof(buf)), BFMC_IMU_GYRO_LENGTH);
    CHECK_EQ(bfmc_imu_gyro_unpack(&out, buf, sizeof(buf)), 0);
    CHECK_EQ(out.gyro_x, in.gyro_x);
    CHECK_EQ(out.gyro_y, in.gyro_y);
    CHECK_EQ(out.gyro_z, in.gyro_z);
}

static void test_battery_roundtrip(void)
{
    struct bfmc_battery_t in;
    struct bfmc_battery_t out;
    uint8_t buf[8];

    in.pack_voltage    = 12345;
    in.pack_current    = -6789;
    in.state_of_charge = 87;
    in.battery_flags   = 0x5A;

    CHECK_EQ(bfmc_battery_pack(buf, &in, sizeof(buf)), BFMC_BATTERY_LENGTH);
    CHECK_EQ(bfmc_battery_unpack(&out, buf, sizeof(buf)), 0);
    CHECK_EQ(out.pack_voltage,    in.pack_voltage);
    CHECK_EQ(out.pack_current,    in.pack_current);
    CHECK_EQ(out.state_of_charge, in.state_of_charge);
    CHECK_EQ(out.battery_flags,   in.battery_flags);
}

static void test_physical_scaling(void)
{
    /* TargetSteerAngle has scale 0.1, so 12.3 deg must land on raw 123.
       Getting this wrong is silent: the vehicle just steers by a factor of 10. */
    CHECK_EQ(bfmc_motion_cmd_target_steer_angle_encode(12.3f), 123);
    CHECK(fabsf(bfmc_motion_cmd_target_steer_angle_decode(123) - 12.3f) < 0.001f);

    CHECK_EQ(bfmc_motion_cmd_target_steer_angle_encode(-25.0f), -250);
    CHECK(fabsf(bfmc_motion_cmd_target_steer_angle_decode(-250) + 25.0f) < 0.001f);

    /* TargetSpeed has scale 1, so encode is the identity. */
    CHECK_EQ(bfmc_motion_cmd_target_speed_encode(1500.0f), 1500);
}

static void test_range_checks(void)
{
    CHECK(bfmc_motion_cmd_target_speed_is_in_range(2000));
    CHECK(bfmc_motion_cmd_target_speed_is_in_range(-2000));
    CHECK(!bfmc_motion_cmd_target_speed_is_in_range(2001));
    CHECK(!bfmc_motion_cmd_target_speed_is_in_range(-2001));

    CHECK(bfmc_motion_cmd_target_steer_angle_is_in_range(250));
    CHECK(!bfmc_motion_cmd_target_steer_angle_is_in_range(251));
}

static void test_undersized_buffer_rejected(void)
{
    struct bfmc_motion_cmd_t in;
    struct bfmc_motion_cmd_t out;
    uint8_t buf[2];

    in.target_speed = 100;
    in.target_steer_angle = 10;
    in.cmd_counter = 1;

    /* A caller passing a short buffer must get an error, never a silent
       truncation onto the bus. */
    CHECK(bfmc_motion_cmd_pack(buf, &in, sizeof(buf)) < 0);
    CHECK(bfmc_motion_cmd_unpack(&out, buf, sizeof(buf)) < 0);
}

static void test_frame_ids_match_matrix(void)
{
    /* Guards against an ID being edited in the DBC without the message matrix
       and the priority scheme being revisited. */
    CHECK_EQ(BFMC_EMERGENCY_STOP_FRAME_ID, 0x010);
    CHECK_EQ(BFMC_SAFETY_STATE_FRAME_ID,   0x080);
    CHECK_EQ(BFMC_MOTION_CMD_FRAME_ID,     0x100);
    CHECK_EQ(BFMC_MOTION_STATUS_FRAME_ID,  0x200);
    CHECK_EQ(BFMC_WHEEL_ENCODER_FRAME_ID,  0x210);
    CHECK_EQ(BFMC_IMU_ACCEL_FRAME_ID,      0x300);
    CHECK_EQ(BFMC_BATTERY_FRAME_ID,        0x340);
    CHECK_EQ(BFMC_HB_MOTION_FRAME_ID,      0x401);
    CHECK_EQ(BFMC_DIAG_RESP_FRAME_ID,      0x700);

    /* Priority ordering is the entire point of the ID scheme. */
    CHECK(BFMC_EMERGENCY_STOP_FRAME_ID < BFMC_MOTION_CMD_FRAME_ID);
    CHECK(BFMC_MOTION_CMD_FRAME_ID     < BFMC_MOTION_STATUS_FRAME_ID);
    CHECK(BFMC_MOTION_STATUS_FRAME_ID  < BFMC_BATTERY_FRAME_ID);
    CHECK(BFMC_BATTERY_FRAME_ID        < BFMC_DIAG_RESP_FRAME_ID);
}

/* ------------------------------------------------------------------ */

static void dump(const char *name, const uint8_t *buf, int len)
{
    int i;
    printf("%s ", name);
    for (i = 0; i < len; i++) {
        printf("%02X", buf[i]);
    }
    printf("\n");
}

/** Fixed vectors, re-encoded independently by cantools in cross_check.py. */
static void dump_vectors(void)
{
    uint8_t buf[8];
    struct bfmc_motion_cmd_t cmd;
    struct bfmc_motion_status_t st;
    struct bfmc_imu_gyro_t gyro;
    struct bfmc_battery_t bat;
    struct bfmc_wheel_encoder_t enc;

    cmd.target_speed = -1500;
    cmd.target_steer_angle = -123;
    cmd.cmd_counter = 200;
    memset(buf, 0, sizeof(buf));
    dump("MOTION_CMD", buf, bfmc_motion_cmd_pack(buf, &cmd, sizeof(buf)));

    st.meas_speed = -874;
    st.meas_steer_angle = 231;
    st.motor_current = -12345;
    st.motion_flags = 0xBEEF;
    memset(buf, 0, sizeof(buf));
    dump("MOTION_STATUS", buf, bfmc_motion_status_pack(buf, &st, sizeof(buf)));

    gyro.gyro_x = -2000;
    gyro.gyro_y = 0;
    gyro.gyro_z = 1999;
    memset(buf, 0, sizeof(buf));
    dump("IMU_GYRO", buf, bfmc_imu_gyro_pack(buf, &gyro, sizeof(buf)));

    bat.pack_voltage = 12345;
    bat.pack_current = -6789;
    bat.state_of_charge = 87;
    bat.battery_flags = 0x5A;
    memset(buf, 0, sizeof(buf));
    dump("BATTERY", buf, bfmc_battery_pack(buf, &bat, sizeof(buf)));

    enc.encoder_ticks = -123456789;
    enc.distance_travelled = 987654321;
    memset(buf, 0, sizeof(buf));
    dump("WHEEL_ENCODER", buf, bfmc_wheel_encoder_pack(buf, &enc, sizeof(buf)));
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump-vectors") == 0) {
        dump_vectors();
        return 0;
    }

    test_motion_cmd_roundtrip();
    test_motion_status_roundtrip();
    test_imu_gyro_roundtrip();
    test_battery_roundtrip();
    test_physical_scaling();
    test_range_checks();
    test_undersized_buffer_rejected();
    test_frame_ids_match_matrix();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
