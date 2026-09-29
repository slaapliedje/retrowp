/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "caps_codec.h"
#include "endian.h"

void caps_encode(const offload_capabilities *caps, u8 *out)
{
    be_write16(out, caps->protocol_version);
    be_write32(out + 2, caps->ops_supported);
    be_write32(out + 6, caps->max_payload_bytes);
    memcpy(out + 10, caps->gateway_name, 32);
}

void caps_decode(const u8 *buf, offload_capabilities *out)
{
    out->protocol_version = be_read16(buf);
    out->ops_supported = be_read32(buf + 2);
    out->max_payload_bytes = be_read32(buf + 6);
    memcpy(out->gateway_name, buf + 10, 32);
}
