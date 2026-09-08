/* Host tests for Services/svc_txframe.c — the per-transport SPSC frame
 * FIFO (length-prefixed frames in a power-of-two byte ring, two-tier
 * urgent/non-urgent admission with a 64-byte reserve). */
#include "test.h"
#include <string.h>

#include "../Services/svc_txframe.c"

#define RING_SZ  256U
static uint8_t  g_storage[RING_SZ];
static SvcTxFrame g_r;

static void fresh(void)
{
    memset(g_storage, 0xAA, sizeof g_storage);
    svc_txframe_init(&g_r, g_storage, RING_SZ);
}

static void fill_pattern(uint8_t *b, uint16_t n, uint8_t seed)
{
    for (uint16_t i = 0; i < n; ++i) b[i] = (uint8_t)(seed + i);
}

TEST(init_is_empty)
{
    fresh();
    CHECK(svc_txframe_is_empty(&g_r));
    CHECK_EQ(svc_txframe_free_bytes(&g_r), RING_SZ - 1U);
}

TEST(push_peek_pop_roundtrip)
{
    fresh();
    uint8_t in[10];  fill_pattern(in, sizeof in, 0x30);
    CHECK(svc_txframe_push(&g_r, in, sizeof in, false));
    CHECK(!svc_txframe_is_empty(&g_r));
    CHECK_EQ(svc_txframe_free_bytes(&g_r), RING_SZ - 1U - (2U + sizeof in));

    uint8_t out[32] = {0};
    CHECK_EQ(svc_txframe_peek(&g_r, out, sizeof out), sizeof in);   /* peek doesn't remove */
    CHECK(memcmp(in, out, sizeof in) == 0);
    CHECK(!svc_txframe_is_empty(&g_r));

    memset(out, 0, sizeof out);
    CHECK_EQ(svc_txframe_pop(&g_r, out, sizeof out), sizeof in);
    CHECK(memcmp(in, out, sizeof in) == 0);
    CHECK(svc_txframe_is_empty(&g_r));
}

TEST(fifo_order)
{
    fresh();
    uint8_t a[10], b[20], c[30];
    fill_pattern(a, sizeof a, 1); fill_pattern(b, sizeof b, 2); fill_pattern(c, sizeof c, 3);
    CHECK(svc_txframe_push(&g_r, a, sizeof a, false));
    CHECK(svc_txframe_push(&g_r, b, sizeof b, false));
    CHECK(svc_txframe_push(&g_r, c, sizeof c, false));

    uint8_t out[40];
    CHECK_EQ(svc_txframe_pop(&g_r, out, sizeof out), sizeof a);  CHECK(memcmp(a, out, sizeof a) == 0);
    CHECK_EQ(svc_txframe_pop(&g_r, out, sizeof out), sizeof b);  CHECK(memcmp(b, out, sizeof b) == 0);
    CHECK_EQ(svc_txframe_pop(&g_r, out, sizeof out), sizeof c);  CHECK(memcmp(c, out, sizeof c) == 0);
    CHECK(svc_txframe_is_empty(&g_r));
}

TEST(non_urgent_stops_at_the_reserve_urgent_may_use_it)
{
    fresh();
    uint8_t f[50];  fill_pattern(f, sizeof f, 0x40);
    /* need = 52, want(non-urgent) = 52 + 64. free starts at 255:
     *   k=1 -> free 203 >= 116  ok
     *   k=2 -> free 151 >= 116  ok
     *   k=3 -> free  99 <  116  REFUSED  */
    CHECK(svc_txframe_push(&g_r, f, sizeof f, false));
    CHECK(svc_txframe_push(&g_r, f, sizeof f, false));
    CHECK(!svc_txframe_push(&g_r, f, sizeof f, false));
    uint16_t free_after = svc_txframe_free_bytes(&g_r);
    CHECK(free_after >= SVC_TXFRAME_RESERVE_BYTES);   /* the reserve is intact */

    /* an urgent frame that fits in what's left (incl. the reserve) is accepted */
    uint8_t u[80];  fill_pattern(u, sizeof u, 0x80);   /* need = 82 <= free_after (151) */
    CHECK(svc_txframe_push(&g_r, u, sizeof u, true));

    /* drain and check the urgent frame is the 3rd out, intact */
    uint8_t out[100];
    CHECK_EQ(svc_txframe_pop(&g_r, out, sizeof out), 50);
    CHECK_EQ(svc_txframe_pop(&g_r, out, sizeof out), 50);
    CHECK_EQ(svc_txframe_pop(&g_r, out, sizeof out), 80);
    CHECK(memcmp(u, out, sizeof u) == 0);
}

TEST(oversized_frame_refused_even_when_urgent)
{
    fresh();
    uint8_t big[200];  memset(big, 7, sizeof big);   /* need 202 > RING_SZ/2 (128) */
    CHECK(!svc_txframe_push(&g_r, big, sizeof big, false));
    CHECK(!svc_txframe_push(&g_r, big, sizeof big, true));
    CHECK(svc_txframe_is_empty(&g_r));
}

TEST(survives_many_wraps)
{
    fresh();
    uint8_t in[100], out[128];
    for (int it = 0; it < 40; ++it) {          /* ~40 * 102 bytes >> RING_SZ: wraps many times */
        fill_pattern(in, sizeof in, (uint8_t)(it * 3 + 1));
        CHECK(svc_txframe_push(&g_r, in, sizeof in, true));
        CHECK_EQ(svc_txframe_pop(&g_r, out, sizeof out), sizeof in);
        CHECK(memcmp(in, out, sizeof in) == 0);
    }
    CHECK(svc_txframe_is_empty(&g_r));
    CHECK_EQ(svc_txframe_free_bytes(&g_r), RING_SZ - 1U);
}

TEST(peek_rejects_too_small_dst)
{
    fresh();
    uint8_t in[40];  fill_pattern(in, sizeof in, 5);
    CHECK(svc_txframe_push(&g_r, in, sizeof in, true));
    uint8_t out[64];
    CHECK_EQ(svc_txframe_peek(&g_r, out, 39), 0);   /* too small: nothing copied, frame stays */
    CHECK(!svc_txframe_is_empty(&g_r));
    CHECK_EQ(svc_txframe_peek(&g_r, out, 40), 40);
}

TEST(rejects_null_and_zero_len)
{
    fresh();
    uint8_t x = 1;
    CHECK(!svc_txframe_push(&g_r, 0, 5, true));
    CHECK(!svc_txframe_push(&g_r, &x, 0, true));
    CHECK(svc_txframe_is_empty(&g_r));
}

TEST(reset_discards_everything)
{
    fresh();
    uint8_t in[20];  fill_pattern(in, sizeof in, 9);
    CHECK(svc_txframe_push(&g_r, in, sizeof in, true));
    CHECK(svc_txframe_push(&g_r, in, sizeof in, true));
    svc_txframe_reset(&g_r);
    CHECK(svc_txframe_is_empty(&g_r));
    CHECK_EQ(svc_txframe_free_bytes(&g_r), RING_SZ - 1U);
}

int main(void)
{
    printf("test_txframe:\n");
    RUN(init_is_empty);
    RUN(push_peek_pop_roundtrip);
    RUN(fifo_order);
    RUN(non_urgent_stops_at_the_reserve_urgent_may_use_it);
    RUN(oversized_frame_refused_even_when_urgent);
    RUN(survives_many_wraps);
    RUN(peek_rejects_too_small_dst);
    RUN(rejects_null_and_zero_len);
    RUN(reset_discards_everything);
    return test_summary();
}
