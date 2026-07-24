#include <string.h>
#include "test.h"
#include "offload.h"
#include "endian.h"
#include "platform.h"
#include "gateway_harness.h"

/* Drives offload/gateway/image_gateway.py (M15 T15.4) through the exact
   same offload_client.c API test_convert.c already proves against
   odt_gateway.py -- fully deterministic (Pillow decode/resize/quantize/
   dither has no LLM in the loop), so this belongs in the automated
   suite, unlike ollama_gateway.c. */

#define GATEWAY_BINARY "offload/gateway/image_gateway.py"
#define FIXTURE_CAP 4096
#define TARGET_GRAYSCALE 0x01

static wp_status read_fixture(const char *path, u8 *buf, u32 cap, u32 *out_len)
{
    plat_file *f;
    wp_status st;

    st = plat_file_open(path, PLAT_FILE_READ, &f);
    if (st != WP_OK) return st;
    st = plat_file_read(f, buf, cap, out_len);
    plat_file_close(f);
    return st;
}

/* Unpacks the nibble at pixel x within a packed 4-bit-indexed row (2
   pixels/byte, high nibble = even x -- docs/formats/offload-protocol.md's
   OFFLOAD_IMAGE section). */
static u8 nibble_at(const u8 *row, u32 x)
{
    u8 b = row[x / 2];
    return (u8)((x % 2 == 0) ? (b >> 4) : (b & 0x0F));
}

/* Finds the palette index whose RGB exactly matches (r,g,b), or 0xFF if
   not present -- avoids assuming any particular palette ORDER out of
   Pillow's quantize(), which this test shouldn't be coupled to. */
static u8 find_palette_index(const u8 *palette, u8 palette_count, u8 r, u8 g, u8 b)
{
    u8 i;

    for (i = 0; i < palette_count; i++) {
        if (palette[i * 3] == r && palette[i * 3 + 1] == g && palette[i * 3 + 2] == b) {
            return i;
        }
    }
    return 0xFF;
}

/* host/tests/fixtures/sample_image.png is a hand-authored 40x30 PNG,
   solid black on its left half (x 0..19) and solid white on its right
   half (x 20..39) -- deliberately just two flat colors, no midtones, so
   neither the adaptive-palette quantize path nor the Floyd-Steinberg
   dither path has anything ambiguous to resolve differently across a
   Pillow version; both must land on exactly black-left/white-right. */
void test_image_convert_real_pillow_color_round_trip(void)
{
    gateway_handle gw;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    u8 png_bytes[FIXTURE_CAP];
    u32 png_len;
    u8 req_payload[FIXTURE_CAP + 6];
    u16 width_px, height_px;
    u8 palette_count;
    const u8 *palette;
    const u8 *pixels;
    u8 black_idx, white_idx;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    ASSERT_TRUE(offload_available());

    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_EQ_STR((const char *)caps.gateway_name, "image-gateway");
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_IMAGE)) != 0);

    ASSERT_TRUE(read_fixture("host/tests/fixtures/sample_image.png",
                              png_bytes, FIXTURE_CAP, &png_len) == WP_OK);

    req_payload[0] = 16; /* target_max_colors */
    req_payload[1] = 0;  /* target_flags: color, not grayscale */
    be_write16(req_payload + 2, 320);
    be_write16(req_payload + 4, 200);
    memcpy(req_payload + 6, png_bytes, png_len);

    req.op = OFFLOAD_IMAGE;
    req.payload = req_payload;
    req.payload_len = png_len + 6;
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);

    /* Well within the 320x200 max -- no downscale, exact dimensions. */
    width_px = be_read16(resp.result);
    height_px = be_read16(resp.result + 2);
    ASSERT_EQ_U32(width_px, 40);
    ASSERT_EQ_U32(height_px, 30);

    palette_count = resp.result[4];
    /* Exactly 2 flat source colors -- quantize(colors=16) must resolve
       to exactly 2 real palette entries, not pad with unused ones. */
    ASSERT_EQ_U32(palette_count, 2);

    palette = resp.result + 5;
    black_idx = find_palette_index(palette, palette_count, 0, 0, 0);
    white_idx = find_palette_index(palette, palette_count, 255, 255, 255);
    ASSERT_TRUE(black_idx != 0xFF);
    ASSERT_TRUE(white_idx != 0xFF);

    pixels = palette + (u32)palette_count * 3;
    ASSERT_EQ_U32(resp.result_len, (u32)(5 + palette_count * 3 + ((width_px + 1) / 2) * height_px));

    /* Row 0: x=0 (left half) must be black, x=39 (right half) white. */
    ASSERT_EQ_U32(nibble_at(pixels, 0), black_idx);
    ASSERT_EQ_U32(nibble_at(pixels, 39), white_idx);
    /* Row 29 (last row): same split holds for every row of a solid
       vertical-half image, not just the first. */
    {
        u32 row_bytes = ((u32)width_px + 1) / 2;
        const u8 *last_row = pixels + row_bytes * (height_px - 1);
        ASSERT_EQ_U32(nibble_at(last_row, 0), black_idx);
        ASSERT_EQ_U32(nibble_at(last_row, 39), white_idx);
    }

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

/* Same fixture, TARGET_GRAYSCALE this time -- real Floyd-Steinberg
   dithering of a pure two-tone source has no midtone to diffuse error
   into, so it must land on exactly the same black-left/white-right
   split as the color path, not an approximation. */
void test_image_convert_real_pillow_grayscale_round_trip(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;
    u8 png_bytes[FIXTURE_CAP];
    u32 png_len;
    u8 req_payload[FIXTURE_CAP + 6];
    u16 width_px, height_px;
    u8 palette_count;
    const u8 *palette;
    const u8 *pixels;
    u8 black_idx, white_idx;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(read_fixture("host/tests/fixtures/sample_image.png",
                              png_bytes, FIXTURE_CAP, &png_len) == WP_OK);

    req_payload[0] = 2; /* target_max_colors -- ignored for grayscale */
    req_payload[1] = TARGET_GRAYSCALE;
    be_write16(req_payload + 2, 320);
    be_write16(req_payload + 4, 200);
    memcpy(req_payload + 6, png_bytes, png_len);

    req.op = OFFLOAD_IMAGE;
    req.payload = req_payload;
    req.payload_len = png_len + 6;
    req.timeout_ms = 10000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);

    width_px = be_read16(resp.result);
    height_px = be_read16(resp.result + 2);
    ASSERT_EQ_U32(width_px, 40);
    ASSERT_EQ_U32(height_px, 30);

    palette_count = resp.result[4];
    ASSERT_EQ_U32(palette_count, 2);

    palette = resp.result + 5;
    black_idx = find_palette_index(palette, palette_count, 0, 0, 0);
    white_idx = find_palette_index(palette, palette_count, 255, 255, 255);
    ASSERT_TRUE(black_idx != 0xFF);
    ASSERT_TRUE(white_idx != 0xFF);

    pixels = palette + (u32)palette_count * 3;
    ASSERT_EQ_U32(nibble_at(pixels, 0), black_idx);
    ASSERT_EQ_U32(nibble_at(pixels, 39), white_idx);

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

/* image_gateway.py's own --force-pillow-unavailable test hook
   deterministically exercises the no-Pillow-installed degrade path
   regardless of whether THIS dev environment actually has Pillow --
   mirroring mock_gateway.c's own fault-injection-flag convention
   (--corrupt-after, --die-after, etc). Both the CAPS bit and a real
   request must reflect "unsupported here", the same honest structural
   degrade odt_gateway.py's own optional fonttools dependency has. */
void test_image_convert_pillow_unavailable_reports_unsupported(void)
{
    gateway_handle gw;
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    static const char *args[] = { "--force-pillow-unavailable", NULL };
    u8 req_payload[7] = { 16, 0, 0x01, 0x40, 0x00, 0xC8, 'x' };

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, args, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_IMAGE)) == 0);

    req.op = OFFLOAD_IMAGE;
    req.payload = req_payload;
    req.payload_len = (u32)sizeof(req_payload);
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_UNSUPPORTED);
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_image_convert_malformed_request_is_error(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;

    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    /* Shorter than the 6-byte header -- no source bytes at all. */
    req.op = OFFLOAD_IMAGE;
    req.payload = (const u8 *)"\x10\x00";
    req.payload_len = 2;
    req.timeout_ms = 5000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_ERR);
    offload_response_free(&resp);

    /* Well-formed header, but the "source bytes" aren't a real image. */
    req.payload = (const u8 *)"\x10\x00\x01\x40\x00\xC8not an image";
    req.payload_len = 19;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_ERR);
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}
