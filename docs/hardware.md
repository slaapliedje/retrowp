# Hardware — the offload side

**None of this is required to run RetroWP.** The engine is fully
functional with no offload device attached; offload features are
enhancements (CLAUDE.md's offload rules). This document exists because
the parts *are* fussy, and because most of what follows was learned the
expensive way on real machines rather than read off a datasheet.

Everything below has been verified end to end: a real Atari Falcon030
dictating over RS-232 into a Raspberry Pi running whisper.cpp, with the
transcribed text inserted at the cursor.

## Bill of materials

| Part | What was used | Notes |
| --- | --- | --- |
| Single-board computer | Raspberry Pi 5 | Any Pi with a 40-pin header works; the UART naming differs, see below |
| RS-232 level shifter | byte2bot Pi Serial Modem HAT (RS-232, DB9) | MAX3232-based. **Inspect the solder joints** — see below |
| Cable | DB9 **null modem**, female–female, full handshake | Pinout below. A 3-wire cable is not enough if you ever want RTS/CTS |
| Microphone | TONOR TC-777 USB | Any USB mic ALSA can see; `arecord -l` must list it |
| Speech model | whisper.cpp + `ggml-base.en.bin` | ~148 MB. Smaller models exist and are faster; larger are slower on a Pi |
| LLM (optional) | ollama | Only for grammar/chat features |

## The serial link

### Cable pinout

A **full-handshake null modem**, continuity-tested:

```
1 -> 1    2 -> 3    3 -> 2    4 -> 6    5 -> 5
6 -> 4    7 -> 8    8 -> 7    9 -> 9
```

Pins 2/3 (data) and 7/8 (RTS/CTS) are the ones that matter. RetroWP runs
`FLOW_NONE` by default, so a 3-wire cable will carry data — but you lose
any option of hardware handshaking later, and you lose the ability to
diagnose a link by watching the handshake lines, which is worth more than
it sounds.

### Pi UART

On a **Pi 5**, the GPIO header UART is `/dev/ttyAMA0`.

> `/dev/serial0` is **not** the right device on a Pi 5 — it points at the
> SoC debug header, not the GPIO pins the HAT sits on. This costs an
> afternoon if you don't know it.

To route CTS/RTS to GPIO16/17 (they are **not** routed by default), add
to `/boot/firmware/config.txt`:

```
dtoverlay=uart0-pi5,ctsrts
```

Verify with `pinctrl`:

```
$ pinctrl get 14-17
14: a4    pn | hi // GPIO14 = TXD0
15: a4    pu | hi // GPIO15 = RXD0
16: a4    pu | lo // GPIO16 = CTS0
17: a4    pn | lo // GPIO17 = RTS0
```

Function `a4` is the UART alternate function — if you see `ip`/`op`
instead, the overlay is not applied. **Polarity is inverted** relative to
the connector: GPIO17 reading `lo` means RTS is *asserted* (the MAX3232
drives the DB9 pin positive). Reading these pins directly is by far the
fastest way to settle "is this end actually driving the line", and it
needs no multimeter.

### Signal levels

A MAX3232 swings roughly **±5.5 V**; Atari's own drivers swing about
**±12 V**. That difference is diagnostically useful: if you probe a pin
and see 12 V, the *Atari* is driving it, not the HAT.

### Inspect the HAT's solder joints

The byte2bot HAT (Rev 0.0) used here shipped with **eight of the nine DB9
pins unsoldered** — the pads were tinned and the pins sat in the holes
making intermittent contact. Every symptom pointed at software.

A continuity tester finds it in a minute: touch the *top* of each pin,
not the pad. If the pad beeps and the pin doesn't, it isn't soldered.

Do this before debugging anything else.

## Atari-specific serial notes

- The **Falcon030's DB9 is SCC channel B**, reached as `Bconmap` device
  7. There is **no device 6** on a Falcon (that numbering belongs to the
  Mega STE/TT); chasing a "Modem 1 vs Modem 2" distinction on a Falcon is
  chasing a port that does not exist.
- `Rsconf()`'s baud table **tops out at 19200**. That is a BIOS limit,
  not a hardware one. Higher rates need HSMODEM or direct SCC register
  programming.
- **Under MiNT, the BIOS `aux:` path will not transmit** on the Falcon's
  SCC port. `Bconout`/`Bcostat` are intercepted by MiNT and routed
  through its own driver, which `Rsconf` never reaches. RetroWP handles
  this automatically: `platform/atari/transport_atari.c` opens MiNT's
  device node (`u:/dev/modem2`) and uses `Fread`/`Fwrite` when one is
  present, falling back to the BIOS path under plain TOS. You do not need
  to configure anything — but if you are writing your own serial test
  program for a Falcon, this is the trap.

## Dictation

```sh
arecord -l                          # the mic must appear here
python3 offload/gateway/whisper_gateway.py \
    --serial-rx /dev/ttyAMA0 --serial-tx /dev/ttyAMA0 \
    --serial-baud 9600 --serial-flow none \
    --audio-device plughw:2,0
```

`--audio-file <wav>` transcribes a fixture instead of recording, which
makes the whole round trip deterministic and needs no microphone — used
by the host tests, and useful for proving the link before trusting the
audio path.

## Diagnostics

Two Atari-side tools ship for bring-up. Neither uses Native Features, so
both run on real hardware (`.word 0x7300` is an illegal instruction on a
real 68030 — every other smoke binary in this project would crash the
machine on launch).

| Tool | Answers |
| --- | --- |
| `make atari-serial-raw` → `SERRAW.PRG` | Protocol-free bytes, each direction separately. Distinguishes "TX dead" from "RX dead" from "both" |
| `make atari-serial-mint` → `SERMINT.PRG` | Reads CTS/DTR/DCD from the Atari's own side via MiNT's `TIOCMGET`, and tries `Fwrite` to the device node. Answers "is the wiring good?" without opening the machine |

`SERMINT`'s CTS reading is the one that splits the search space: CTS low
means the far end's RTS is not arriving and no software change will help;
CTS high means the wiring is fine and the fault is in software.
