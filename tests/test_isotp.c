#include "test_util.h"

#include "isotp.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* A fake CAN sink, so emitted frames can be inspected                  */
/* ------------------------------------------------------------------ */

#define SINK_MAX 64

typedef struct
{
    uint8_t frame[SINK_MAX][8];
    uint8_t dlc[SINK_MAX];
    int     count;
    bool    refuse;
} sink_t;

static bool sink_send(void *ctx, const uint8_t *data, uint8_t dlc)
{
    sink_t *s = (sink_t *)ctx;

    if (s->refuse || s->count >= SINK_MAX) {
        return false;
    }
    memcpy(s->frame[s->count], data, dlc);
    s->dlc[s->count] = dlc;
    s->count++;
    return true;
}

static void setup(isotp_t *tp, sink_t *s)
{
    memset(s, 0, sizeof(*s));
    IsoTp_Init(tp, sink_send, s);
}

static void make_ff(uint8_t *f, uint16_t declared, uint8_t fill)
{
    int i;
    f[0] = (uint8_t)(0x10U | ((declared >> 8) & 0x0FU));
    f[1] = (uint8_t)(declared & 0xFFU);
    for (i = 2; i < 8; i++) {
        f[i] = fill;
    }
}

static void make_cf(uint8_t *f, uint8_t sn, uint8_t fill)
{
    int i;
    f[0] = (uint8_t)(0x20U | (sn & 0x0FU));
    for (i = 1; i < 8; i++) {
        f[i] = fill;
    }
}

/* ------------------------------------------------------------------ */
/* Happy path                                                          */
/* ------------------------------------------------------------------ */

static void test_single_frame(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8] = { 0x03, 0x22, 0xF1, 0x90, 0, 0, 0, 0 };
    const uint8_t *msg; uint16_t len = 0;

    setup(&tp, &s);

    CHECK(IsoTp_OnFrame(&tp, f, 8, 0));
    msg = IsoTp_Message(&tp, &len);
    CHECK(msg != NULL);
    CHECK_EQ(len, 3);
    CHECK_EQ(msg[0], 0x22);
    CHECK_EQ(msg[2], 0x90);
    CHECK_EQ(tp.stat_rx_messages, 1);
}

static void test_segmented_message_reassembles(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];
    const uint8_t *msg; uint16_t len = 0;
    uint8_t sn;

    setup(&tp, &s);

    /* 20 bytes: 6 in the first frame, then 7 + 7. */
    make_ff(f, 20, 0xAA);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));

    /* The receiver must answer a first frame with flow control, or the sender
       will never send the rest. */
    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.frame[0][0] >> 4, ISOTP_PCI_FLOW_CONTROL);
    CHECK_EQ(s.frame[0][0] & 0x0F, ISOTP_FS_CONTINUE);

    sn = 1;
    make_cf(f, sn++, 0xBB);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 10));
    make_cf(f, sn++, 0xCC);
    CHECK(IsoTp_OnFrame(&tp, f, 8, 20));

    msg = IsoTp_Message(&tp, &len);
    CHECK(msg != NULL);
    CHECK_EQ(len, 20);
    CHECK_EQ(msg[0], 0xAA);   /* from the first frame  */
    CHECK_EQ(msg[6], 0xBB);   /* first consecutive     */
    CHECK_EQ(msg[13], 0xCC);  /* second consecutive    */
    CHECK_EQ(tp.stat_rx_messages, 1);
}

static void test_sequence_number_wraps(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];
    int i;

    setup(&tp, &s);

    /* 21 consecutive frames: the 4-bit sequence number wraps through zero. */
    make_ff(f, 150, 0x01);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));

    for (i = 0; i < 21; i++) {
        uint8_t sn = (uint8_t)((i + 1) & 0x0F);
        bool done;
        make_cf(f, sn, (uint8_t)i);
        done = IsoTp_OnFrame(&tp, f, 8, (uint32_t)(10 * i));
        if (i < 20) {
            CHECK(!done);
        } else {
            CHECK(done);
        }
    }
    CHECK_EQ(tp.stat_rx_seq_errors, 0);
    CHECK_EQ(tp.stat_rx_messages, 1);
}

/* ------------------------------------------------------------------ */
/* Hostile and malformed input                                         */
/* ------------------------------------------------------------------ */

static void test_declared_length_beyond_buffer_is_refused(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];

    setup(&tp, &s);

    /* A first frame may declare 4095 bytes. Rejected before any copy, and
     * signalled to the sender rather than silently truncated. */
    make_ff(f, ISOTP_MAX_DECLARED, 0xFF);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));

    CHECK_EQ(tp.last_error, ISOTP_ERR_TOO_LONG);
    CHECK_EQ(tp.stat_rx_overflow, 1);
    CHECK_EQ(tp.state, ISOTP_IDLE);
    CHECK_EQ(tp.rx_received, 0);

    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.frame[0][0] >> 4, ISOTP_PCI_FLOW_CONTROL);
    CHECK_EQ(s.frame[0][0] & 0x0F, ISOTP_FS_OVERFLOW);
}

/* The index bound is asserted directly: overflowing rx_buf first corrupts
 * rx_received right after it, which ends the loop before a guard region is
 * reached. The guard stays as a secondary check. */
static void test_overflow_attempt_stays_inside_the_buffer(void)
{
    struct {
        isotp_t tp;
        uint8_t guard[64];
    } g;
    sink_t s;
    uint8_t f[8];
    int i;
    bool guard_intact = true;
    bool index_in_bounds = true;

    setup(&g.tp, &s);
    memset(g.guard, 0xC5, sizeof(g.guard));

    make_ff(f, ISOTP_MAX_DECLARED, 0xFF);
    (void)IsoTp_OnFrame(&g.tp, f, 8, 0);

    /* Declare the maximum, then push far more than the buffer holds. */
    for (i = 0; i < 700; i++) {
        make_cf(f, (uint8_t)((i + 1) & 0x0F), 0xFF);
        (void)IsoTp_OnFrame(&g.tp, f, 8, (uint32_t)i);

        if ((g.tp.rx_received > (uint16_t)ISOTP_MAX_MESSAGE) ||
            (g.tp.rx_expected > (uint16_t)ISOTP_MAX_MESSAGE)) {
            index_in_bounds = false;
        }
    }

    CHECK(index_in_bounds);

    for (i = 0; i < (int)sizeof(g.guard); i++) {
        if (g.guard[i] != 0xC5) {
            guard_intact = false;
        }
    }
    CHECK(guard_intact);

    CHECK(IsoTp_Message(&g.tp, NULL) == NULL);
    CHECK_EQ(g.tp.stat_rx_messages, 0);
}

static void test_exactly_buffer_sized_is_accepted(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];

    setup(&tp, &s);

    /* The boundary must still work. */
    make_ff(f, (uint16_t)ISOTP_MAX_MESSAGE, 0x5A);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));

    CHECK_EQ(tp.last_error, ISOTP_ERR_NONE);
    CHECK_EQ(tp.stat_rx_overflow, 0);
    CHECK_EQ(tp.state, ISOTP_RECEIVING);
    CHECK_EQ(s.frame[0][0] & 0x0F, ISOTP_FS_CONTINUE);
}

static void test_one_byte_over_buffer_is_refused(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];

    setup(&tp, &s);
    make_ff(f, (uint16_t)(ISOTP_MAX_MESSAGE + 1U), 0x5A);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));

    CHECK_EQ(tp.last_error, ISOTP_ERR_TOO_LONG);
    CHECK_EQ(tp.stat_rx_overflow, 1);
}

static void test_out_of_order_frame_abandons_the_message(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];

    setup(&tp, &s);

    make_ff(f, 20, 0xAA);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));

    /* Never assemble a message from pieces of two transfers. */
    make_cf(f, 3, 0xBB);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 10));

    CHECK_EQ(tp.last_error, ISOTP_ERR_BAD_SEQUENCE);
    CHECK_EQ(tp.stat_rx_seq_errors, 1);
    CHECK_EQ(tp.state, ISOTP_IDLE);
    CHECK(IsoTp_Message(&tp, NULL) == NULL);
}

static void test_orphan_consecutive_frame_is_ignored(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];

    setup(&tp, &s);

    /* Someone else's traffic: counted, ignored, and no reassembly started. */
    make_cf(f, 1, 0xBB);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));

    CHECK_EQ(tp.stat_rx_orphans, 1);
    CHECK_EQ(tp.state, ISOTP_IDLE);
    CHECK_EQ(tp.rx_received, 0);
}

static void test_first_frame_declaring_short_length_is_malformed(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];

    setup(&tp, &s);

    /* <= 7 bytes should have been a single frame. */
    make_ff(f, 4, 0xAA);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));

    CHECK_EQ(tp.last_error, ISOTP_ERR_MALFORMED);
    CHECK_EQ(tp.stat_malformed, 1);
    CHECK_EQ(tp.state, ISOTP_IDLE);
}

static void test_single_frame_length_must_fit_the_frame(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];

    setup(&tp, &s);

    /* Claims seven bytes, four arrived. */
    memset(f, 0, sizeof(f));
    f[0] = 0x07;
    CHECK(!IsoTp_OnFrame(&tp, f, 4, 0));
    CHECK_EQ(tp.last_error, ISOTP_ERR_MALFORMED);

    f[0] = 0x00;
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));
    CHECK_EQ(tp.stat_malformed, 2);
}

static void test_empty_frame_is_rejected(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8] = { 0 };

    setup(&tp, &s);
    CHECK(!IsoTp_OnFrame(&tp, f, 0, 0));
    CHECK_EQ(tp.last_error, ISOTP_ERR_MALFORMED);
}

static void test_new_first_frame_preempts_reassembly(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];
    const uint8_t *msg; uint16_t len = 0;

    setup(&tp, &s);

    make_ff(f, 100, 0xAA);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 0));
    make_cf(f, 1, 0xAA);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 10));

    /* The new message must not inherit bytes from the old. */
    make_ff(f, 13, 0xBB);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 20));
    CHECK_EQ(tp.rx_expected, 13);
    CHECK_EQ(tp.rx_received, 6);

    make_cf(f, 1, 0xBB);
    CHECK(IsoTp_OnFrame(&tp, f, 8, 30));

    msg = IsoTp_Message(&tp, &len);
    CHECK_EQ(len, 13);
    CHECK_EQ(msg[0], 0xBB);
    CHECK_EQ(msg[12], 0xBB);
}

static void test_stalled_transfer_times_out(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];

    setup(&tp, &s);

    /* Otherwise a silent peer holds the buffer forever. */
    make_ff(f, 100, 0xAA);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, 1000));

    IsoTp_Poll(&tp, 1000 + ISOTP_N_CR_TIMEOUT_MS - 1);
    CHECK_EQ(tp.state, ISOTP_RECEIVING);   /* still inside the budget */

    IsoTp_Poll(&tp, 1000 + ISOTP_N_CR_TIMEOUT_MS);
    CHECK_EQ(tp.state, ISOTP_IDLE);
    CHECK_EQ(tp.last_error, ISOTP_ERR_TIMEOUT);
    CHECK_EQ(tp.stat_rx_timeouts, 1);
}

static void test_timeout_survives_tick_wraparound(void)
{
    isotp_t tp; sink_t s;
    uint8_t f[8];
    const uint32_t near_wrap = 0xFFFFFFF0UL;

    setup(&tp, &s);
    make_ff(f, 100, 0xAA);
    CHECK(!IsoTp_OnFrame(&tp, f, 8, near_wrap));

    IsoTp_Poll(&tp, near_wrap + ISOTP_N_CR_TIMEOUT_MS - 1);
    CHECK_EQ(tp.state, ISOTP_RECEIVING);

    IsoTp_Poll(&tp, near_wrap + ISOTP_N_CR_TIMEOUT_MS);
    CHECK_EQ(tp.state, ISOTP_IDLE);
    CHECK_EQ(tp.last_error, ISOTP_ERR_TIMEOUT);
}

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

static void test_short_message_sends_as_single_frame(void)
{
    isotp_t tp; sink_t s;
    uint8_t msg[3] = { 0x62, 0xF1, 0x90 };

    setup(&tp, &s);
    CHECK(IsoTp_Send(&tp, msg, sizeof(msg), 0));

    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.frame[0][0], 0x03);
    CHECK_EQ(s.frame[0][1], 0x62);
    CHECK_EQ(tp.state, ISOTP_IDLE);   /* nothing left pending */
}

static void test_long_message_segments_after_flow_control(void)
{
    isotp_t tp; sink_t s;
    uint8_t msg[20];
    uint8_t fc[3];
    int i;

    for (i = 0; i < 20; i++) {
        msg[i] = (uint8_t)i;
    }

    setup(&tp, &s);
    CHECK(IsoTp_Send(&tp, msg, sizeof(msg), 0));

    /* Only the first frame goes out; the rest waits for permission. */
    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.frame[0][0], 0x10);
    CHECK_EQ(s.frame[0][1], 20);
    CHECK_EQ(tp.state, ISOTP_TRANSMITTING);

    fc[0] = 0x30; fc[1] = 0x00; fc[2] = 0x00;  /* continue, no block limit */
    (void)IsoTp_OnFrame(&tp, fc, 3, 10);
    IsoTp_Poll(&tp, 10);

    CHECK_EQ(s.count, 3);                 /* first frame + two consecutive */
    CHECK_EQ(s.frame[1][0], 0x21);
    CHECK_EQ(s.frame[2][0], 0x22);
    CHECK_EQ(tp.state, ISOTP_IDLE);
}

static void test_overflow_flow_control_aborts_the_send(void)
{
    isotp_t tp; sink_t s;
    uint8_t msg[20];
    uint8_t fc[3];

    memset(msg, 0xAA, sizeof(msg));
    setup(&tp, &s);
    CHECK(IsoTp_Send(&tp, msg, sizeof(msg), 0));

    /* Overflow status: stop sending. */
    fc[0] = 0x32; fc[1] = 0x00; fc[2] = 0x00;
    (void)IsoTp_OnFrame(&tp, fc, 3, 10);

    CHECK_EQ(tp.state, ISOTP_IDLE);
    CHECK_EQ(tp.last_error, ISOTP_ERR_TOO_LONG);
    CHECK_EQ(s.count, 1);   /* nothing further was sent */
}

static void test_send_rejects_while_busy(void)
{
    isotp_t tp; sink_t s;
    uint8_t msg[20];

    memset(msg, 0x11, sizeof(msg));
    setup(&tp, &s);

    CHECK(IsoTp_Send(&tp, msg, sizeof(msg), 0));
    CHECK(!IsoTp_Send(&tp, msg, sizeof(msg), 0));  /* one at a time */
}

static void test_send_rejects_beyond_protocol_limit(void)
{
    isotp_t tp; sink_t s;
    static uint8_t big[ISOTP_MAX_DECLARED + 2];

    setup(&tp, &s);
    CHECK(!IsoTp_Send(&tp, big, (uint16_t)(ISOTP_MAX_DECLARED + 1U), 0));
    CHECK_EQ(s.count, 0);
}

static void test_full_mailboxes_do_not_lose_data(void)
{
    isotp_t tp; sink_t s;
    uint8_t msg[20];
    uint8_t fc[3];
    int i;

    for (i = 0; i < 20; i++) {
        msg[i] = (uint8_t)i;
    }

    setup(&tp, &s);
    CHECK(IsoTp_Send(&tp, msg, sizeof(msg), 0));

    fc[0] = 0x30; fc[1] = 0x00; fc[2] = 0x00;
    (void)IsoTp_OnFrame(&tp, fc, 3, 10);

    /* Every mailbox busy: retry, do not drop the bytes and advance. */
    s.refuse = true;
    IsoTp_Poll(&tp, 10);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(tp.state, ISOTP_TRANSMITTING);

    s.refuse = false;
    IsoTp_Poll(&tp, 20);
    CHECK_EQ(s.count, 3);
    CHECK_EQ(s.frame[1][0], 0x21);   /* sequence resumed, not skipped */
    CHECK_EQ(s.frame[1][1], 6);      /* byte 6 is where it left off   */
    CHECK_EQ(tp.state, ISOTP_IDLE);
}

int main(void)
{
    test_single_frame();
    test_segmented_message_reassembles();
    test_sequence_number_wraps();

    test_declared_length_beyond_buffer_is_refused();
    test_overflow_attempt_stays_inside_the_buffer();
    test_exactly_buffer_sized_is_accepted();
    test_one_byte_over_buffer_is_refused();
    test_out_of_order_frame_abandons_the_message();
    test_orphan_consecutive_frame_is_ignored();
    test_first_frame_declaring_short_length_is_malformed();
    test_single_frame_length_must_fit_the_frame();
    test_empty_frame_is_rejected();
    test_new_first_frame_preempts_reassembly();
    test_stalled_transfer_times_out();
    test_timeout_survives_tick_wraparound();

    test_short_message_sends_as_single_frame();
    test_long_message_segments_after_flow_control();
    test_overflow_flow_control_aborts_the_send();
    test_send_rejects_while_busy();
    test_send_rejects_beyond_protocol_limit();
    test_full_mailboxes_do_not_lose_data();

    return test_report();
}
