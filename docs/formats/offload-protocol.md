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

### Any other op

The mock gateway responds `RESPONSE` with `status = OFFLOAD_UNSUPPORTED`
and an empty payload for any op it doesn't implement (`GRAMMAR_CHECK`,
`CONVERT_IMPORT`, `CONVERT_EXPORT`, `CHAT`, `IMAGE`) -- exercising the
"absent gracefully" capability-negotiation path that M8's optional ops
need later, before any of them actually exist.

## Mock Gateway (`offload/gateway/mock_gateway.c`)

A standalone POSIX executable, built to `bin/mock_gateway`, never linked
into the engine or app. It answers THESAURUS/SPELL_CHECK from a small
fixture table (`offload/gateway/fixtures.h`/`.c`) over a real TCP socket,
so `host/tests/test_offload.c` can prove the protocol round-trips
deterministically without a Pi or an LLM. It binds TCP port 0 (OS-
assigned) and prints the assigned port as one line on stdout before
accepting, so `host/tests/gateway_harness.c` never has to guess a port
or poll for readiness.

Fault-injection CLI flags, each mapped to a specific client-observable
failure mode exercised by `test_offload.c`:

| Flag | Behavior | Exercises |
|---|---|---|
| `--corrupt-after=N` | Flips a byte in the CRC of every response after the Nth (0 = first response) | CRC rejection + resulting timeout, over a real socket |
| `--die-after=N` | `_exit(1)` after the Nth request instead of responding | Client detects a dead peer mid-request, doesn't hang |
| `--delay-ms=N` | Sleeps N ms before every response | Deterministic `WP_TIMEOUT` with no race on process timing |
| `--short-frame` | Sends a response header promising a large payload, writes a few bytes, then goes back to waiting for the next request | Client times out on a stalled/incomplete frame, distinct from CRC corruption |

Real LLM/dictionary integration (Ollama/llama.cpp on a Pi) replacing
`fixtures.c` is explicit future scope (M8) -- **for a real
GRAMMAR_CHECK/full SPELL_CHECK gateway.** `ollama_gateway.c` below is a
narrower, already-implemented slice of that scope: one real op
(THESAURUS), not the full M8 feature set.

## Ollama Gateway (`offload/gateway/ollama_gateway.c`, post-M6)

A second standalone executable, built to `bin/ollama_gateway`, speaking
the exact same wire protocol/handshake as `mock_gateway.c` (both share
`gw_common.c` for the transport-agnostic plumbing -- frame send/receive,
FIFO opening, word-list encoding -- so the two never drift). The
difference is entirely in what backs one op:

- **`OFFLOAD_THESAURUS`** is answered by a real call to a local Ollama
  server (`http://127.0.0.1:11434` by default) instead of a fixture
  table -- genuine LLM-generated synonyms, not canned data.
- **Every other op, including `OFFLOAD_SPELL_CHECK`,** responds
  `OFFLOAD_UNSUPPORTED` -- a deliberate choice, not a missing feature.
  Tested against this gateway's default model (`deepseek-coder-v2:16b`,
  the only non-"thinking" model available in this project's own dev
  environment), a spelling probe for `teh` came back "Correct" -- a
  worse answer than `services/spell`'s deterministic on-device checker
  already gives for free. THESAURUS has a much lower correctness bar (a
  real-vocabulary word list has value even when incomplete) and is
  exactly the kind of large-vocabulary knowledge CLAUDE.md's own project
  brief names as offload-worthy, so that's where this gateway spends its
  one real capability. A gateway that does GRAMMAR_CHECK/SPELL_CHECK for
  real, likely against a better-suited model, is M8 scope.

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
