/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "platform.h"
#include "offload.h"
#include "timer_host.h"
#include "gateway_harness.h"

/* T20.7: deadlines across the u32 wrap of plat_ticks_ms. */

/* Stands the clock `ms_before` short of the wrap. */
static void clock_before_wrap(u32 ms_before)
{
    u32 raw;

    timer_host_set_bias(0);
    raw = plat_ticks_ms();
    timer_host_set_bias((u32)0 - raw - ms_before);
}

void test_ticks_deadline_holds_across_the_wrap(void)
{
    u32 t0;

    clock_before_wrap(100);
    t0 = plat_ticks_ms();
    ASSERT_TRUE(t0 > 0xFFFFFF00uL);          /* really near the top */
    ASSERT_TRUE(!PLAT_TICKS_PASSED(t0, 1000));
    /* What the old test, now >= t0 + timeout, said here: t0 + 1000 has
       already wrapped to a small number, so the deadline was "passed"
       before any time had gone by. */
    ASSERT_TRUE(plat_ticks_ms() >= (u32)(t0 + 1000u));

    plat_sleep_ms(300);                       /* across the wrap */
    ASSERT_TRUE(plat_ticks_ms() < t0);       /* it did wrap */
    ASSERT_TRUE(PLAT_TICKS_PASSED(t0, 250));
    ASSERT_TRUE(!PLAT_TICKS_PASSED(t0, 1000));
    ASSERT_TRUE(PLAT_TICKS_SINCE(t0) >= 300u && PLAT_TICKS_SINCE(t0) < 1000u);
    timer_host_set_bias(0);
}

/* The offload client's one deadline, crossing the wrap: a request that
   is answered still succeeds, and one that is not answered times out
   after its timeout -- not at once, not never. */
void test_ticks_offload_deadline_across_the_wrap(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;
    static const char *slow[] = { "--delay-ms=1500", NULL };
    u32 t0, took;

    req.op = OFFLOAD_THESAURUS;
    req.payload = (const u8 *)"happy";
    req.payload_len = 5;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    clock_before_wrap(5);
    req.timeout_ms = 2000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);

    ASSERT_TRUE(gateway_start(slow, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    clock_before_wrap(200);
    req.timeout_ms = 600;
    t0 = plat_ticks_ms();
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_TIMEOUT);
    took = PLAT_TICKS_SINCE(t0);
    ASSERT_TRUE(took >= 550u && took < 1400u);
    offload_shutdown();
    gateway_stop(&gw);
    timer_host_set_bias(0);
}

/* T20.7: a write to a peer that has gone is WP_ERR, never SIGPIPE --
   whose default action would end this whole test run. The host write
   polls first and usually sees the dead peer as POLLHUP/POLLERR, so this
   passed before the fix too; what the fix closes is the race of the
   peer's reset landing between that poll and the write(). Kept as the
   contract's guard. */
void test_ticks_write_to_a_dead_peer_is_an_error(void)
{
    gateway_handle gw;
    plat_transport *t = NULL;
    static u8 junk[4096];
    wp_status st = WP_OK;
    int i;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(plat_transport_open(gw.endpoint, 2000, &t) == WP_OK);
    gateway_kill(&gw);
    for (i = 0; i < 64 && st == WP_OK; i++) st = plat_transport_write(t, junk, sizeof(junk), 500);
    ASSERT_TRUE(st == WP_ERR);
    plat_transport_close(t);
}

/* Data before hang-up (platform.h). The host read tested POLLHUP and
   POLLERR before POLLIN, so a peer whose last bytes were followed by a
   hang-up lost them: WP_ERR with the bytes still unread. On Linux an
   ordinary close (FIN) shows only POLLIN|POLLRDHUP, so the case to
   test is an abortive close (RST, SO_LINGER 0) -- POLLIN|POLLHUP|POLLERR
   with the data still readable, as a probe showed. Other systems report
   POLLHUP for more kinds of close. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>

void test_transport_delivers_data_before_hangup(void)
{
    int lfd, cfd;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    char endpoint[64];
    plat_transport *t = NULL;
    u8 buf[16];
    u32 got = 0;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(lfd >= 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_TRUE(bind(lfd, (struct sockaddr *)&a, sizeof(a)) == 0);
    ASSERT_TRUE(listen(lfd, 1) == 0);
    ASSERT_TRUE(getsockname(lfd, (struct sockaddr *)&a, &alen) == 0);
    sprintf(endpoint, "tcp:127.0.0.1:%d", (int)ntohs(a.sin_port));

    ASSERT_TRUE(plat_transport_open(endpoint, 2000, &t) == WP_OK);
    cfd = accept(lfd, NULL, NULL);
    ASSERT_TRUE(cfd >= 0);
    ASSERT_TRUE(write(cfd, "tail", 4) == 4);
    plat_sleep_ms(50);          /* the bytes arrive first */
    {
        struct linger lg;
        lg.l_onoff = 1;
        lg.l_linger = 0;
        ASSERT_TRUE(setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
    }
    close(cfd);                 /* then a reset: POLLHUP|POLLERR with them */
    close(lfd);
    plat_sleep_ms(100);         /* both have arrived before the poll */

    ASSERT_TRUE(plat_transport_read(t, buf, sizeof(buf), 500, &got) == WP_OK);
    ASSERT_EQ_U32(got, 4);
    ASSERT_TRUE(memcmp(buf, "tail", 4) == 0);
    ASSERT_TRUE(plat_transport_read(t, buf, sizeof(buf), 500, &got) == WP_ERR);
    plat_transport_close(t);
}
