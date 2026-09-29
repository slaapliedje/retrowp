/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_GATEWAY_HTTP_CLIENT_H
#define WP_GATEWAY_HTTP_CLIENT_H

#include "types.h"

/* Minimal, single-purpose HTTP/1.1 client: one POST, one response, no
   libcurl dependency (offload/gateway's own rule of thumb, same reason
   the engine avoids dependencies beyond mem_alloc -- one raw TCP socket
   and a hand-written request/response is genuinely simpler than a build-
   system dependency for something this narrow). Not a general HTTP
   library: no redirects, no chunked transfer-encoding, no keep-alive --
   ollama_gateway.c's only caller always talks to a local Ollama server
   that answers a non-streaming JSON POST with a plain Content-Length
   body, which is all this supports. */

/* POSTs body (body_len bytes, Content-Type: application/json) to
   http://host:port/path and reads the response until the peer closes
   the connection (this always sends "Connection: close"). `host` must
   be a dotted-decimal IPv4 address, not a hostname -- there is no DNS
   resolution here, matching the "one local caller, one narrow job"
   scope. On WP_OK, *out_len is the number of BODY bytes (headers
   stripped) copied into out (out_cap bytes, NOT NUL-terminated by this
   function -- callers needing a C string must add one themselves if
   out_len < out_cap). Response status is not checked (a non-2xx JSON
   error body from Ollama is still returned to the caller, which can
   inspect it) -- only whether bytes were readable at all.
   WP_TIMEOUT if connect/send/recv exceeds timeout_ms. WP_ERR on any
   other failure (connection refused, malformed response, response body
   larger than out_cap, no blank-line header/body separator found). */
wp_status http_post_json(const char *host, u16 port, const char *path,
                          const u8 *body, u32 body_len, u32 timeout_ms,
                          u8 *out, u32 out_cap, u32 *out_len);

#endif /* WP_GATEWAY_HTTP_CLIENT_H */
