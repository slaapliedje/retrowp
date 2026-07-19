#include <stddef.h>
#include "platform.h"

/* No RS232/MiNTnet transport yet (T6.3, blocked on this M4 backend
   existing at all -- see docs/milestones.md). Per CLAUDE.md's Offload
   Rules, the engine must be fully functional WITHOUT an offload device
   attached: offload_client.c's offload_available() must return false
   and every request must fail fast rather than hang, which this stub
   guarantees by construction. */

wp_status plat_transport_open(const char *endpoint, u32 connect_timeout_ms,
                               plat_transport **out)
{
    WP_UNUSED(endpoint);
    WP_UNUSED(connect_timeout_ms);
    if (out) *out = NULL;
    return WP_UNSUPPORTED;
}

wp_status plat_transport_read(plat_transport *t, u8 *out, u32 out_cap,
                               u32 timeout_ms, u32 *out_len)
{
    WP_UNUSED(t); WP_UNUSED(out); WP_UNUSED(out_cap); WP_UNUSED(timeout_ms);
    if (out_len) *out_len = 0;
    return WP_ERR;
}

wp_status plat_transport_write(plat_transport *t, const u8 *data, u32 len,
                                u32 timeout_ms)
{
    WP_UNUSED(t); WP_UNUSED(data); WP_UNUSED(len); WP_UNUSED(timeout_ms);
    return WP_ERR;
}

void plat_transport_close(plat_transport *t)
{
    WP_UNUSED(t);
}
