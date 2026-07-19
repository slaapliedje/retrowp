# Offload Protocol

> Seeds the fuller `protocol.md` that M7 (offloaded ODT/PDF formats) will
> need. Covers everything implemented as of M6: the wire frame, the
> HELLO/CAPS handshake, and the THESAURUS/SPELL_CHECK payload shapes.

## Transport

The protocol is transport-agnostic: it runs identically over `pipe:`
(host self-test), `tcp:host:port` (host, and the mock gateway), and a
future `serial:device:baud` (Atari RS232, M6.3 -- not yet implemented).
Framing, CRC, and payload shapes below never change based on transport;
only `plat_transport_open`'s endpoint string does. See
`engine/include/platform.h` and `offload/transport/wire.h` for where this
seam lives.

## Wire Frame

All multi-byte fields big-endian (CLAUDE.md rule 3: never a raw struct
dump). Implemented in `offload/transport/wire.h`/`wire.c`.

```
offset  size  field
0       1     magic0     0x57 ('W')
1       1     magic1     0x50 ('P')
2       1     version    1
3       1     type       wire_msg_type: HELLO=0, CAPS=1, REQUEST=2, RESPONSE=3, ERROR=4
4       1     op         offload_op (meaningful for REQUEST/RESPONSE, else 0)
5       1     status     offload_status (meaningful for RESPONSE/ERROR, else 0)
6       2     seq        matches a RESPONSE to the REQUEST that triggered it
8       4     length     payload byte length, <= 65536 (WIRE_MAX_PAYLOAD)
12      N     payload    N = length bytes, opaque to the framing layer
12+N    2     crc16      CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) over
                          bytes [version .. end of payload]
```

Total frame size = 14 + length.

**Resync policy:** a frame that fails validation (wrong version, length
above the max, or a CRC mismatch) is discarded, and scanning resumes from
the next unread stream byte. `wire_read_frame` never seeks backward --
`plat_transport_read` is a forward-only, unbuffered read with no
pushback -- so a garbage `length` field can in principle cause part of a
following real frame to be skipped along with it; recovery happens as
soon as the next `magic0, magic1` pair is found in the stream. Two
independent budgets bound how long this can go on against a truly
garbage stream (total bytes skipped scanning for magic; total frames
rejected after a full header was read); exceeding either returns
`WP_ERR`, meaning "reconnect," not "keep retrying this stream." See the
comment above `wire_read_frame` in `wire.h` for the full contract.

## Handshake: HELLO / CAPS

Immediately after `plat_transport_open` succeeds, the client
(`offload_init` in `engine/src/offload_client.c`) sends a zero-payload
`HELLO` frame and waits for a `CAPS` response.

`CAPS` payload (42 bytes, big-endian), implemented by
`offload/transport/caps_codec.h`/`.c` -- shared verbatim by the client
and the gateway so neither has to depend on the other's build:

```
offset  size  field
0       2     protocol_version
2       4     ops_supported      bitmask, bit (1uL << offload_op)
6       4     max_payload_bytes
10      32    gateway_name       UTF-8, NUL-padded
```

If the handshake fails for any reason (no device, connection refused,
timeout, malformed CAPS), `offload_init` returns the underlying error and
`offload_available()` reports false from then on -- the engine remains
fully functional (CLAUDE.md "Offload Rules"): offload features are
enhancements, never a hard dependency.

## Request / Response Payloads

`offload_request.payload`/`offload_response.result` are opaque `u8*`
blobs as far as `offload_client.c` and the wire layer are concerned --
neither parses op-specific shapes. The shapes below are the CONTRACT
between a caller of `offload_submit` and whatever gateway is on the other
end (the mock gateway today; a real dictionary/LLM-backed gateway is
M8).

### `OFFLOAD_THESAURUS`

- **Request payload:** the UTF-8 word, no framing beyond the wire
  frame's own `length` field.
- **Response payload:** a length-prefixed word list --
  ```
  offset  size  field
  0       2     count
  2       ...   count x (u16 len, len bytes of UTF-8)
  ```
  An unknown word gets `count == 0` (an empty list), not an error --
  "no synonyms found" is a valid answer.

### `OFFLOAD_SPELL_CHECK`

- **Request payload:** the UTF-8 word, same as THESAURUS.
- **Response payload:**
  ```
  offset  size  field
  0       1     is_correct   0 or 1
  1       2     count        suggestion count
  3       ...   count x (u16 len, len bytes of UTF-8)
  ```
  Same length-prefixed word-list shape as THESAURUS, after the
  `is_correct` byte.

### `OFFLOAD_CONVERT_IMPORT` / `OFFLOAD_CONVERT_EXPORT` (M7)

**Design decision: RTF is the wire-level interchange format, not the
internal document model.** `format_rtf_import`/`format_rtf_export`
already exist, are tested, and cross-compile for every real target
(M5) -- reusing them means CONVERT_IMPORT/EXPORT need no new engine-side
serialization of `wp_document` at all. The engine always sends/receives
RTF bytes across this op; only the GATEWAY needs to know how to turn a
foreign format into RTF and back. This also bounds what a gateway needs
to preserve on a round trip to exactly the subset `docs/formats/rtf.md`
already documents (alignment, indents, spacing, bold/italic/underline/
strike/super/sub, size, color) -- the same honest, narrow contract every
other format in this project has, not a new one invented for M7.

- **`CONVERT_IMPORT` request payload:**
  ```
  offset  size  field
  0       1     format_id (1 = ODT)
  1       ...   the foreign file's raw bytes (e.g. a whole .odt file)
  ```
  **Response payload (status `OFFLOAD_OK`):** RTF bytes, exactly the
  shape `format_rtf_import` accepts. The engine's own Open-ODT flow (a
  future task) is therefore just: ship the file's bytes with
  `format_id=1`, then feed the response straight into
  `format_rtf_import()` -- no new engine-side parsing at all.
- **`CONVERT_EXPORT` request payload:**
  ```
  offset  size  field
  0       1     format_id (1 = ODT, 2 = PDF)
  1       ...   RTF bytes, produced by the engine's OWN format_rtf_export()
  ```
  **Response payload (status `OFFLOAD_OK`):** the foreign format's raw
  bytes (a whole `.odt` file, or a whole `.pdf` file) -- write directly
  to disk, no further engine-side work.
  PDF is **export-only**: there is no `format_id` for "import a PDF" --
  PDF has no general, reliable text-reconstruction guarantee (real-world
  PDFs range from clean text to scanned images with no text layer at
  all), and nothing in this project needs to read one back in.
- A gateway that implements CONVERT_IMPORT/EXPORT only needs to
  understand RTF in the ONE shape `format_rtf_export` actually produces
  (documented exactly in `docs/formats/rtf.md`'s "Export" section) and
  only needs to PRODUCE RTF in the shape `format_rtf_import` actually
  accepts -- not general third-party RTF in either direction, since the
  wire contract above guarantees which side of the conversion always
  touches this project's own RTF code. `offload/gateway/odt_gateway.py`
  (below) takes advantage of this deliberately.

### `OFFLOAD_GRAMMAR_CHECK` (M8 T8.1)

**Design decision: the gateway proposes, the gateway itself verifies --
never trust a model's claimed position.** An LLM asked for "the issue"
in a paragraph is good at picking out a real, verbatim phrase and good
at explaining what's wrong with it, but is NOT reliably able to count
bytes/characters to produce a correct numeric offset (this is a well
known weak spot for token-based models, not specific to any one model
tried here). So the wire response's `start_offset` is never a number
the model produced -- the gateway asks the model to quote the exact
offending text instead, then finds that quote in the ORIGINAL request
paragraph itself via a real, deterministic byte-for-byte substring
search, computing the offset/length from that match. A quote the
gateway can't find verbatim in the original text (a paraphrase, or an
outright hallucination) is silently dropped, never forwarded -- the
same "don't propagate garbage" convention `parse_word_list` already
uses for THESAURUS. This means every issue the client ever receives is
guaranteed to point at real text that is genuinely present in the
paragraph it was asked about, independent of how faithfully any given
model followed instructions.

- **Request payload:** the UTF-8 paragraph text, no framing beyond the
  wire frame's own `length` field -- same convention as THESAURUS/
  SPELL_CHECK.
- **Response payload:** a length-prefixed issue list --
  ```
  offset  size  field
  0       2     issue_count
  2       ...   issue_count x issue record:
                  2     start_offset    byte offset into the request
                                        paragraph (from the real
                                        substring search above, never
                                        the model's own count)
                  2     span_len        byte length of the flagged span
                  2     message_len
                  ...   message_len bytes of UTF-8 (short explanation)
                  2     suggestion_len
                  ...   suggestion_len bytes of UTF-8 (replacement
                                        text; suggestion_len == 0 means
                                        "flagged, no fix offered")
  ```
  A clean paragraph gets `issue_count == 0`, not an error -- "no issues
  found" is a valid answer, same convention as an empty THESAURUS/
  SPELL_CHECK list.
- Byte offsets, not codepoint offsets (`skills/encoding.md`'s policy --
  bytes are the wire/storage unit; codepoint-aware stepping is a
  presentation-layer concern). Safe by construction here: `span_len`
  bytes starting at `start_offset` are always an exact copy of
  contiguous bytes from the original (valid UTF-8) paragraph, so the
  span can never land mid-codepoint.

### Any other op

The mock gateway responds `RESPONSE` with `status = OFFLOAD_UNSUPPORTED`
and an empty payload for any op it doesn't implement (`CHAT`, `IMAGE`,
and -- for `mock_gateway.c`/`ollama_gateway.c` specifically --
`CONVERT_IMPORT`/`CONVERT_EXPORT` too, since ODT/PDF conversion lives in
the separate `odt_gateway.py`, not either of those; `ollama_gateway.c`
also reports `OFFLOAD_UNSUPPORTED` for `SPELL_CHECK`, see below) --
exercising the "absent gracefully" capability-negotiation path M8's
still-optional ops (`CHAT`/`IMAGE`) need, before either actually exists.

## Mock Gateway (`offload/gateway/mock_gateway.c`)

A standalone POSIX executable, built to `bin/mock_gateway`, never linked
into the engine or app. It answers THESAURUS/SPELL_CHECK/GRAMMAR_CHECK
from a small fixture table (`offload/gateway/fixtures.h`/`.c`) over a
real TCP socket, so `host/tests/test_offload.c` can prove the protocol
round-trips deterministically without a Pi or an LLM -- GRAMMAR_CHECK's
fixture table maps a small set of known fixture paragraphs to canned
issue lists (already-computed offsets, no substring search needed since
the fixture data is trusted by construction), covering the wire
encode/decode shape without needing a real model in the loop. It binds
TCP port 0 (OS-assigned) and prints the assigned port as one line on
stdout before accepting, so `host/tests/gateway_harness.c` never has to
guess a port or poll for readiness.

Fault-injection CLI flags, each mapped to a specific client-observable
failure mode exercised by `test_offload.c`:

| Flag | Behavior | Exercises |
|---|---|---|
| `--corrupt-after=N` | Flips a byte in the CRC of every response after the Nth (0 = first response) | CRC rejection + resulting timeout, over a real socket |
| `--die-after=N` | `_exit(1)` after the Nth request instead of responding | Client detects a dead peer mid-request, doesn't hang |
| `--delay-ms=N` | Sleeps N ms before every response | Deterministic `WP_TIMEOUT` with no race on process timing |
| `--short-frame` | Sends a response header promising a large payload, writes a few bytes, then goes back to waiting for the next request | Client times out on a stalled/incomplete frame, distinct from CRC corruption |

Real LLM/dictionary integration (Ollama/llama.cpp on a Pi) replacing
`fixtures.c` is explicit future scope (M8) -- **for a real full
SPELL_CHECK gateway** (GRAMMAR_CHECK is now covered, see below).
`ollama_gateway.c` below is a narrower, already-implemented slice of
that scope: two real ops (THESAURUS, GRAMMAR_CHECK), not the full M8
feature set (SPELL_CHECK/CHAT/IMAGE remain unimplemented by any gateway
in this repo).

## Ollama Gateway (`offload/gateway/ollama_gateway.c`, post-M6)

A second standalone executable, built to `bin/ollama_gateway`, speaking
the exact same wire protocol/handshake as `mock_gateway.c` (both share
`gw_common.c` for the transport-agnostic plumbing -- frame send/receive,
FIFO opening, word-list encoding -- so the two never drift). The
difference is entirely in what backs one op:

- **`OFFLOAD_THESAURUS`** is answered by a real call to a local Ollama
  server (`http://127.0.0.1:11434` by default) instead of a fixture
  table -- genuine LLM-generated synonyms, not canned data. Default
  model `deepseek-coder-v2:16b` (`--model` to override), unchanged since
  M6 -- see below for why GRAMMAR_CHECK deliberately does NOT reuse this
  default.
- **`OFFLOAD_GRAMMAR_CHECK`** (T8.1) is answered by a real call to the
  same Ollama server, but with its OWN default model, `gemma2:9b`
  (`--grammar-model` to override, independent of `--model`). This was an
  empirical choice, not a guess: prompted with a paragraph containing
  several real errors (subject-verb agreement, "alot", "their" for
  "they're"), `gemma2:9b` reliably followed the rigid
  `QUOTE :: MESSAGE :: SUGGESTION` output line format this gateway's
  parser depends on, and every quote it produced was a genuine, exact
  substring of the source paragraph. `mistral:7b`, tried the same way
  (including a retry with a few-shot example line added to the prompt),
  never produced a clean 3-field line either time, and on the retry
  hallucinated words that do not appear anywhere in the source paragraph
  at all. Two different models loaded means paying Ollama's cold-load
  cost separately the first time each op is used after a gap -- an
  accepted cost of keeping Thesaurus's already-verified model choice
  untouched rather than picking one model to serve both tasks.
- **Every other op, including `OFFLOAD_SPELL_CHECK`,** responds
  `OFFLOAD_UNSUPPORTED` -- a deliberate choice, not a missing feature.
  Tested against `deepseek-coder-v2:16b` during M6, a spelling probe for
  `teh` came back "Correct" -- a worse answer than `services/spell`'s
  deterministic on-device checker already gives for free, and the
  reason SPELL_CHECK stays on-device rather than getting the same
  offload treatment THESAURUS and GRAMMAR_CHECK just did.

**No libcurl.** `offload/gateway/http_client.c` hand-rolls the single
HTTP/1.1 POST this needs directly over a raw socket (`http_post_json()`):
build the request line/headers/JSON body, send, read until the peer
closes (`Connection: close`, so no chunked-encoding handling is needed),
split headers from body on the first blank line. Not a general HTTP
client -- no redirects, no keep-alive, no DNS (`--ollama-host` must be a
dotted-decimal IPv4 address). This keeps the same "no dependencies
beyond what's truly needed" posture the rest of `offload/gateway` and
the engine already hold, and mirrors `wire.c`'s own framing-only,
zero-I/O design philosophy.

**Prompting and parsing.** The request asks the model for "ONLY a
comma-separated list... no numbering, no explanation"; the response is
extracted from Ollama's `"response":"..."` JSON field (a small hand-
rolled extractor, not a JSON library -- `extract_json_string_field()`
handles the handful of escapes (`\"`, `\\`, `\n`, `\r`, `\t`) that can
appear in a plain-text answer and drops `\uXXXX` escapes rather than
decoding them, matching the ASCII-first scope `services/spell` already
established). The extracted text is then split on commas/newlines,
trimmed, and filtered to tokens that look like a single word (letters,
hyphen, apostrophe only) -- silently dropping anything else (a trailing
empty token, a stray sentence the model added despite instructions).
Verified against a live model (see below): `"happy"` reliably returns a
clean list like `cheerful, joyful, content, pleased, merry, ...`.

**Honesty note on "unknown word" behavior.** `fixtures.c`'s mock gateway
guarantees `count == 0` for a word it doesn't recognize (a static table
lookup). This gateway makes NO such guarantee: tested against a genuine
nonsense word (`zzqxwibble`), the model returned nothing usable on one
run (empty-token list, correctly decoded to `count == 0`) but
*hallucinated* eight plausible-looking "synonyms" on another run against
the same gateway build. Both are valid wire responses (the protocol
never required correctness, only a well-formed answer), but this is a
real, inherent difference from the deterministic fixture table, not a
bug in the parser -- worth knowing before wiring a UI action that
displays results uncritically.

**Resource cost, measured, not assumed.** In this project's own dev
environment, a cold model load (Ollama had unloaded it after its
`keep_alive` window) took ~20 real seconds for the default 8.9GB model
before the first token came back, even on an otherwise-idle GPU -- hence
`--ollama-timeout-ms` defaults to 60000, not something tuned for an
already-warm model. A 32B "thinking" model also present in that same
environment was tried first and abandoned for this default: under
concurrent load from an unrelated GPU-heavy process, a single request
did not return within several minutes and was killed rather than waited
out. `--model` is a plain CLI override precisely so a differently-
equipped machine, or a smaller/faster instruct model pulled later, isn't
locked to what happened to work in one environment.

**CLI flags:** `--ollama-host=` (default `127.0.0.1`), `--ollama-port=`
(default `11434`), `--model=` (default `deepseek-coder-v2:16b`),
`--ollama-timeout-ms=` (default `60000`), `--hello-timeout-ms=` (default
`5000`, same meaning as `mock_gateway.c`'s flag of the same name), and
the same `--serial-rx=`/`--serial-tx=` pair `mock_gateway.c` supports,
for driving this over a real Hatari RS-232 bridge exactly like T6.3.

**Verification.** Deliberately kept OUT of `make test`/`ci.sh`: LLM
output is not deterministic, and a contributor without Ollama installed
must still be able to run the automated suite. Instead:
- `host/tools/ollama_gateway_smoke.c` drives the REAL `offload_client.c`
  API (`offload_init`/`offload_query`/`offload_submit`/
  `offload_shutdown`) -- the same code any real menu action would call
  -- against a spawned `ollama_gateway`, asserting the structural
  properties that ARE deterministic (handshake succeeds, negotiated
  `gateway_name` is `"ollama-gateway"`, `OFFLOAD_THESAURUS` is
  advertised, the request returns `OFFLOAD_OK`) while printing the actual
  synonym list for a human to eyeball.
- `host/tools/run_ollama_smoke.sh` spawns `bin/ollama_gateway`, captures
  its OS-assigned port the same way `gateway_harness.c` already does for
  the mock gateway's tests, and runs the smoke tool against it. Run by
  hand: `host/tools/run_ollama_smoke.sh happy`.
- Also proven directly over the raw wire protocol (a throwaway Python
  client, the same style of tool used to isolate T6.3's SIGPIPE bug) for
  three cases in one session: a real word (`"happy"` &rarr; 8 real
  synonyms), an unsupported op (`SPELL_CHECK` &rarr; `OFFLOAD_UNSUPPORTED`,
  empty payload), and multiple sequential requests over one connection
  (proving the request/response loop, not just a single round trip).

## ODT Gateway (`offload/gateway/odt_gateway.py`, M7 T7.2)

A third gateway, answering `CONVERT_IMPORT`/`CONVERT_EXPORT` for
`format_id = FORMAT_ODT` by converting to/from the RTF interchange shape
described above. Unlike `ollama_gateway.c`, this one IS fully
deterministic and IS part of `make test` (`host/tests/test_convert.c`).
Full design, the two real parsing bugs found and fixed during
development, and the verification approach (both automated and manual
LibreOffice cross-validation) are in `docs/formats/odt.md` -- read that
before changing this gateway.

The same process also answers `CONVERT_EXPORT` for `format_id =
FORMAT_PDF` (M7 T7.3, `docs/formats/pdf.md`) -- a real, from-scratch PDF
writer with its own text layout (word-wrap, pagination, per-line
positioning against real Adobe AFM glyph metrics), since unlike ODT, a
PDF has no importer downstream to defer layout to. `FORMAT_PDF` is
export-only; there is no PDF import.
