/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200112L   /* setenv, kill */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "test.h"
#include "platform.h"
#include "offload.h"
#include "gateway_harness.h"

/* OFFLOAD_SET_SPEED over a real tty pair: socat links two ptys, the
   Python gateway (wire_protocol.py's serve) holds one at 9600, the
   client opens the other as "serial:<pty>:9600". A pty has no wire, so
   this proves the protocol -- offer, pick, switch, confirm or fall back
   -- not the electrical speed; the targets' own switching is theirs. */

#define GW "offload/gateway/odt_gateway.py"

typedef struct {
    int socat_pid;
    char a[96], b[96];
} pty_pair;

static wp_bool pty_pair_open(pty_pair *pp)
{
    char la[128], lb[128];
    int i, pid;

    snprintf(pp->a, sizeof(pp->a), "build/tests/pty-%d-a", (int)getpid());
    snprintf(pp->b, sizeof(pp->b), "build/tests/pty-%d-b", (int)getpid());
    snprintf(la, sizeof(la), "pty,raw,echo=0,link=%s", pp->a);
    snprintf(lb, sizeof(lb), "pty,raw,echo=0,link=%s", pp->b);
    unlink(pp->a);
    unlink(pp->b);
    pid = (int)fork();
    if (pid < 0) return WP_FALSE;
    if (pid == 0) {
        execlp("socat", "socat", la, lb, (char *)NULL);
        _exit(127);
    }
    pp->socat_pid = pid;
    for (i = 0; i < 50; i++) {
        if (access(pp->a, F_OK) == 0 && access(pp->b, F_OK) == 0) return WP_TRUE;
        plat_sleep_ms(100);
    }
    return WP_FALSE;
}

static void pty_pair_close(pty_pair *pp)
{
    if (pp->socat_pid > 0) {
        kill(pp->socat_pid, SIGTERM);
        waitpid(pp->socat_pid, NULL, 0);
        pp->socat_pid = 0;
    }
    unlink(pp->a);
    unlink(pp->b);
}

/* A small request after the switch: the link must still carry one. */
static wp_bool request_works(void)
{
    static const char rtf[] = "\x01{\\rtf1\\ansi Speed test.\\par}";
    offload_request req;
    offload_response resp;
    wp_bool ok;

    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = (const u8 *)rtf;
    req.payload_len = (u32)(sizeof(rtf) - 1);
    req.timeout_ms = 10000;
    if (offload_submit(&req, &resp) != WP_OK) return WP_FALSE;
    ok = (wp_bool)(resp.status == OFFLOAD_OK && resp.result_len > 4 &&
                   resp.result[0] == 'P' && resp.result[1] == 'K');
    offload_response_free(&resp);
    return ok;
}

static void run_case(const char *max_arg, wp_bool make_it_fail, u32 want_speed)
{
    pty_pair pp;
    gateway_handle gw;
    char rx[160], tx[160], endpoint[160];
    const char *args[6];

    memset(&pp, 0, sizeof(pp));
    if (!pty_pair_open(&pp)) {
        printf("  SKIP: socat could not make a pty pair\n");
        pty_pair_close(&pp);
        return;
    }
    snprintf(rx, sizeof(rx), "--serial-rx=%s", pp.a);
    snprintf(tx, sizeof(tx), "--serial-tx=%s", pp.a);
    args[0] = rx; args[1] = tx; args[2] = "--serial-baud=9600";
    args[3] = max_arg; args[4] = NULL; args[5] = NULL;
    if (make_it_fail) setenv("RETROWP_GATEWAY_SPEED_FAIL", "1", 1);
    ASSERT_TRUE(gateway_start_bin(GW, args, &gw) == WP_OK);
    if (make_it_fail) unsetenv("RETROWP_GATEWAY_SPEED_FAIL");

    snprintf(endpoint, sizeof(endpoint), "serial:%s:9600", pp.b);
    ASSERT_TRUE(offload_init(endpoint) == WP_OK);
    ASSERT_EQ_U32(offload_link_speed(), want_speed);
    ASSERT_TRUE(request_works());

    offload_shutdown();
    gateway_stop(&gw);
    pty_pair_close(&pp);
}

/* Both ends can do 115200: the link moves up from 9600, and carries a
   request there. */
void test_link_speed_moves_up(void)
{
    run_case("--serial-max-baud=115200", WP_FALSE, 115200uL);
}

/* The new speed is not confirmed (the gateway's test hook ignores the
   HELLO there): both go back, and the link works at 9600. */
void test_link_speed_falls_back(void)
{
    run_case("--serial-max-baud=115200", WP_TRUE, 9600uL);
}

/* The gateway caps it: the client offers 115200 first, and gets 38400. */
void test_link_speed_gateway_caps_it(void)
{
    run_case("--serial-max-baud=38400", WP_FALSE, 38400uL);
}

/* This side's own limit (Settings' Speed, offload_set_speed_limit): a
   gateway that would go to 115200 is offered nothing past 57600. */
void test_link_speed_own_limit(void)
{
    offload_set_speed_limit(57600uL);
    run_case("--serial-max-baud=115200", WP_FALSE, 57600uL);
    offload_set_speed_limit(0);
}

/* The program run again after a session that moved up: the gateway is
   still at 115200, the new client starts at 9600. On a real wire its
   HELLO arrives as noise; a pty carries bytes whatever the speed, so the
   noise is written here by hand, as the wire would deliver it. The
   gateway must hear it as a client starting over, go back to 9600, and
   move up again with it -- not stay at 115200 while the client, told
   "unsupported", stays at 9600, which on a wire is a dead link. */
void test_link_speed_restart_finds_the_gateway(void)
{
    static const u8 noise[16] = { 0, 0x80, 0, 0xF8, 0, 0, 0x80, 0xFE,
                                  0, 0x80, 0, 0xF8, 0, 0, 0x80, 0xFE };
    pty_pair pp;
    gateway_handle gw;
    char rx[160], tx[160], endpoint[160];
    const char *args[5];
    int fd;

    memset(&pp, 0, sizeof(pp));
    if (!pty_pair_open(&pp)) {
        printf("  SKIP: socat could not make a pty pair\n");
        pty_pair_close(&pp);
        return;
    }
    snprintf(rx, sizeof(rx), "--serial-rx=%s", pp.a);
    snprintf(tx, sizeof(tx), "--serial-tx=%s", pp.a);
    args[0] = rx; args[1] = tx; args[2] = "--serial-baud=9600";
    args[3] = "--serial-max-baud=115200"; args[4] = NULL;
    ASSERT_TRUE(gateway_start_bin(GW, args, &gw) == WP_OK);

    snprintf(endpoint, sizeof(endpoint), "serial:%s:9600", pp.b);
    ASSERT_TRUE(offload_init(endpoint) == WP_OK);
    ASSERT_EQ_U32(offload_link_speed(), 115200uL);
    offload_shutdown();

    fd = open(pp.b, O_RDWR | O_NOCTTY);
    ASSERT_TRUE(fd >= 0);
    if (fd >= 0) {
        ASSERT_TRUE(write(fd, noise, sizeof(noise)) == (ssize_t)sizeof(noise));
        close(fd);
    }
    plat_sleep_ms(300);

    ASSERT_TRUE(offload_init(endpoint) == WP_OK);
    ASSERT_EQ_U32(offload_link_speed(), 115200uL);
    ASSERT_TRUE(request_works());

    offload_shutdown();
    gateway_stop(&gw);
    pty_pair_close(&pp);
}
