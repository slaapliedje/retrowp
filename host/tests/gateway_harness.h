#ifndef WP_GATEWAY_HARNESS_H
#define WP_GATEWAY_HARNESS_H

#include "types.h"

/* fork/exec helper for driving offload/gateway/mock_gateway (built to
   bin/mock_gateway) from host tests, so test_offload.c can exercise
   engine/src/offload_client.c over a real TCP socket against a real
   (if fixture-backed) peer process -- not just the in-memory framing
   already covered by test_wire.c. */

typedef struct {
    int  pid;         /* -1 once stopped/killed */
    int  port;
    char endpoint[64]; /* "tcp:127.0.0.1:<port>", for offload_init */
} gateway_handle;

/* Spawns bin/mock_gateway with the given extra CLI flags (a NULL-
   terminated array of strings, e.g. {"--delay-ms=500", NULL}; pass NULL
   for no extra flags), waits for it to report its assigned TCP port, and
   fills in handle->endpoint. Returns WP_OK, or WP_ERR if the gateway
   couldn't be started or never reported a port within a few seconds. */
wp_status gateway_start(const char *const *extra_args, gateway_handle *handle);

/* SIGKILLs the gateway process immediately and reaps it. Used both as a
   fault-injection tool (simulating a hard crash) and as end-of-test
   cleanup. Safe to call on an already-dead handle. */
void gateway_kill(gateway_handle *handle);

/* Graceful end-of-test cleanup: SIGTERMs and reaps the process if it's
   still running. Safe to call after gateway_kill, or on a gateway that
   already exited on its own (e.g. after its idle-read timeout). */
void gateway_stop(gateway_handle *handle);

#endif /* WP_GATEWAY_HARNESS_H */
