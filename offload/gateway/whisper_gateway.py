#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Standalone dictation gateway (T174).

A fifth offload/gateway executable, alongside mock_gateway.c
(fixture-backed), ollama_gateway.c (real-LLM-backed), odt_gateway.py
(ODT/PDF conversion) and image_gateway.py (image downconversion).
Answers the real OFFLOAD_DICTATE op: the editor says "listen", this
gateway records on ITS OWN microphone, transcribes locally, and returns
UTF-8 text. Everything else responds OFFLOAD_UNSUPPORTED.

THE AUDIO NEVER CROSSES THE LINK. That is the entire reason this op is
viable on a 1MB Atari ST over a 9600-baud serial line: the microphone,
the model and the compute all live on the gateway, so the request is 4
fixed bytes and the response is just the transcript. A dictated sentence
is ~100 bytes of UTF-8, about 0.05s of wire time at 19200 baud -- the
link is nowhere near the bottleneck, the model is. Sending PCM the other
way would be hopeless (16kHz mono S16_LE is 32000 bytes/second, roughly
17x faster than 19200 baud can carry), which is exactly why the design
puts the capture on this side.

Wire format (docs/formats/offload-protocol.md's OFFLOAD_DICTATE section):
  Request:  max_duration_s(1) flags(1) language(2)
  Response: UTF-8 transcript bytes (an EMPTY payload with STATUS_OK
            means "listened, heard nothing" -- a normal outcome, not an
            error, so the caller leaves the document untouched)

Backed by whisper.cpp (ggml), not openai-whisper, on purpose:
  - pure C/C++ with no torch dependency. The Pi ships Python 3.13, where
    aarch64 torch wheels are a coin flip, and a hard torch dependency is
    the wrong shape for a project whose baseline target is a 1MB ST.
  - measured on the real Pi 5 (4x Cortex-A76, CPU only) against 11s of
    speech: ggml-tiny.en 1.22s, ggml-base.en 2.67s, ggml-small.en 7.72s.
    base.en is the default here -- 4x realtime, so a 5-second dictated
    sentence transcribes in about 1.2s.

NOT on the Hailo-10H NPU, despite that being the AI HAT actually fitted.
The runtime binds fine (/dev/hailo0, firmware 5.1.1, arch HAILO10H) but
Raspberry Pi's `hailo-models` package ships vision HEFs only (yolo*,
resnet, scrfd) -- there is no ASR model for this part, and compiling one
needs Hailo's x86-only, license-gated Dataflow Compiler. CPU is fast
enough that this is a real ship, not a stopgap; if a HAILO10H Whisper
HEF ever appears it swaps in behind this same op with no protocol,
engine or backend change at all.

Dependencies are gated exactly like image_gateway.py's optional Pillow:
checked at startup, with OFFLOAD_DICTATE's CAPS bit set only when the
whisper binary, the model file, and a capture path are all actually
present. A gateway that cannot hear reports the op as unsupported rather
than failing per-request, so the backend's own CAPS pre-check (T8.3
CHAT's discipline) keeps the menu item honest.
"""

import array
import os
import re
import shutil
import subprocess
import sys
import tempfile
import wave

from wire_protocol import (
    STATUS_OK, STATUS_UNSUPPORTED, STATUS_ERR,
    OP_DICTATE,
    parse_gateway_args, run_gateway_with_args,
)

# Whisper's own native input format. Chosen to match rather than convert:
# the KTMicro USB capture device on the real Pi already delivers exactly
# this via ALSA's plughw, so there is no resampling stage anywhere.
SAMPLE_RATE = 16000
SAMPLE_WIDTH = 2          # S16_LE
CHANNELS = 1

CHUNK_MS = 100
CHUNK_SAMPLES = SAMPLE_RATE * CHUNK_MS // 1000
CHUNK_BYTES = CHUNK_SAMPLES * SAMPLE_WIDTH

# Measured noise floor on the real device with nothing plugged into the
# mic jack was peak amplitude 58 (RMS well under that), so these sit far
# enough above it that an idle room never reads as speech.
SPEECH_RMS = 400
SILENCE_RMS = 250
TRAILING_SILENCE_MS = 1200

# Speech must be SUSTAINED before it counts, and early-stop is refused
# until a minimum window has elapsed. Both exist because of a real
# failure: a single 100ms chunk of noise latched "speech detected", the
# next 1.2s of quiet then satisfied the trailing-silence rule, and the
# whole recording ended after ~2 seconds -- before the writer had said
# anything. One click should not end a dictation.
SPEECH_CHUNKS_REQUIRED = 3      # 300ms of continuous above-threshold audio
MIN_LISTEN_MS = 2500            # never stop early inside this window

# Discard the first 800ms entirely -- not analysed, not recorded.
#
# MEASURED on the real capture device (TONOR TC-777, quiet room): opening
# the stream produces a settling transient that reads far above any
# plausible speech threshold before decaying to the true noise floor.
#   per-100ms RMS: 3866 2588 1770 1222 810 545 375 252 199 127 99 85 73 ...
#                  |<------- transient, 6 chunks over 400 ------->|
#                                          true floor is 60-85
# SPEECH_CHUNKS_REQUIRED alone cannot filter this, because the transient
# IS sustained -- it satisfied a 3-consecutive-chunk rule easily, latched
# "speech detected" before the speaker had said anything, and the
# recording then ended on the following silence. Twice.
WARMUP_CHUNKS = 8

WP_DICTATE_MAX_DURATION_S = 60
WP_DICTATE_DEFAULT_DURATION_S = 15

FLAG_TRIM_SILENCE = 0x01

DEFAULT_WHISPER_CLI = os.path.expanduser('~/whisper.cpp/build/bin/whisper-cli')
DEFAULT_WHISPER_MODEL = os.path.expanduser('~/whisper.cpp/models/ggml-base.en.bin')


def _detect_capture_device():
    """First ALSA capture card, as `plughw:<card>,<device>`.

    Detected rather than hardcoded because a hardcoded name has already
    broken once here: the default used to be `plughw:CARD=Audio,DEV=0`,
    which stopped resolving the moment the microphone was swapped (the
    KTMicro enumerates its card as `Audio`, a TONOR TC-777 as `Device`).
    Card NUMBERS move too when USB devices are re-plugged, so neither
    half of that name is stable -- detecting at startup is the only
    version that survives the user changing microphones, which on a
    dictation feature is a thing they will obviously do.

    `plughw:` rather than `hw:` so ALSA converts sample format/rate if a
    device won't do 16kHz mono S16_LE natively."""
    try:
        out = subprocess.run(['arecord', '-l'], capture_output=True,
                              timeout=10).stdout.decode('utf-8', 'replace')
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r'^card (\d+):.*?device (\d+):', out, re.MULTILINE | re.DOTALL)
    if not m:
        return None
    return 'plughw:%s,%s' % (m.group(1), m.group(2))


def _rms(chunk):
    """Root-mean-square of one S16_LE chunk.

    Hand-rolled rather than via `audioop`, which PEP 594 REMOVED in
    Python 3.13 -- and 3.13.5 is what the real Pi runs. At 10 chunks a
    second over 1600 samples this is nowhere near hot enough to care."""
    samples = array.array('h')
    samples.frombytes(chunk[:len(chunk) - (len(chunk) % 2)])
    if not samples:
        return 0
    total = 0
    for s in samples:
        total += s * s
    return int((total / len(samples)) ** 0.5)


def _record(device, max_duration_s, trim_silence):
    """Capture raw PCM from `device`, returning bytes.

    Stops at max_duration_s, or early once TRAILING_SILENCE_MS of quiet
    follows speech that was actually detected. Early stop matters for
    the UX: the alternative is making the writer sit through the full
    timeout after every sentence, and the ST has no way to render a
    live level meter that would make a manual stop feel responsive."""
    if not device:
        return b''
    cmd = ['arecord', '-D', device, '-f', 'S16_LE',
           '-r', str(SAMPLE_RATE), '-c', str(CHANNELS), '-t', 'raw', '-q']
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    captured = bytearray()
    max_chunks = (max_duration_s * 1000) // CHUNK_MS
    silence_chunks_needed = TRAILING_SILENCE_MS // CHUNK_MS
    min_chunks = MIN_LISTEN_MS // CHUNK_MS
    heard_speech = False
    consecutive_speech = 0
    trailing_silence = 0
    elapsed_chunks = 0

    try:
        # Drain the device's settling transient before anything counts.
        for _ in range(WARMUP_CHUNKS):
            if not proc.stdout.read(CHUNK_BYTES):
                break

        for _ in range(max_chunks):
            chunk = proc.stdout.read(CHUNK_BYTES)
            if not chunk:
                break
            captured += chunk
            elapsed_chunks += 1

            level = _rms(chunk)
            if level >= SPEECH_RMS:
                consecutive_speech += 1
                if consecutive_speech >= SPEECH_CHUNKS_REQUIRED:
                    heard_speech = True
                trailing_silence = 0
            else:
                consecutive_speech = 0
                if heard_speech and level < SILENCE_RMS:
                    trailing_silence += 1
                    if (trim_silence
                            and elapsed_chunks >= min_chunks
                            and trailing_silence >= silence_chunks_needed):
                        break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    return bytes(captured) if heard_speech else b''


def _is_non_speech_marker(text):
    """True when whisper produced ONLY annotation, no speech.

    whisper.cpp emits bracketed markers for audio it can hear but cannot
    transcribe -- '[BLANK_AUDIO]', '[MUSIC]', '(silence)' and friends.
    These are NOT transcript. A real run returned exactly '[BLANK_AUDIO]',
    which the gateway would otherwise have handed to the caller and the
    caller would have typed into the writer's document at the cursor.

    Deliberately conservative: this returns True only when the ENTIRE
    transcript consists of bracketed groups. Text with any content
    outside brackets is passed through completely untouched, so a
    genuine dictated parenthetical survives intact -- stripping markers
    out of real speech would be a worse bug than the one being fixed."""
    stripped = text.strip()
    if not stripped:
        return True
    remainder = re.sub(r'[\[\(][^\]\)]*[\]\)]', '', stripped)
    return remainder.strip() == ''


def _write_wav(path, pcm):
    with wave.open(path, 'wb') as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(SAMPLE_WIDTH)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)


def _transcribe(cli, model, wav_path, language, threads):
    cmd = [cli, '-m', model, '-f', wav_path, '-nt', '-np', '-t', str(threads)]
    if language:
        cmd += ['-l', language]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=300)
    except (subprocess.TimeoutExpired, OSError):
        return None
    if out.returncode != 0:
        return None
    # -nt -np leaves just the transcript, but whisper.cpp still emits a
    # leading space per segment and a trailing newline.
    text = ' '.join(out.stdout.decode('utf-8', 'replace').split())
    return '' if _is_non_speech_marker(text) else text


def handle_dictate(payload, cfg):
    if not cfg['available']:
        return STATUS_UNSUPPORTED, b''
    if len(payload) < 4:
        return STATUS_ERR, b''

    max_duration_s = payload[0] or WP_DICTATE_DEFAULT_DURATION_S
    flags = payload[1]
    lang_raw = payload[2:4]

    if max_duration_s > WP_DICTATE_MAX_DURATION_S:
        max_duration_s = WP_DICTATE_MAX_DURATION_S
    trim_silence = bool(flags & FLAG_TRIM_SILENCE)

    # 0x0000 means "auto-detect"; anything else is two ASCII letters.
    language = ''
    if lang_raw != b'\x00\x00':
        try:
            candidate = lang_raw.decode('ascii').strip('\x00 ').lower()
            if candidate.isalpha():
                language = candidate
        except UnicodeDecodeError:
            return STATUS_ERR, b''

    if cfg['audio_file']:
        wav_path = cfg['audio_file']
        cleanup = None
    else:
        pcm = _record(cfg['device'], max_duration_s, trim_silence)
        if not pcm:
            # Listened, heard nothing. A normal outcome, not an error:
            # the caller inserts nothing and says so on the status line.
            return STATUS_OK, b''
        fd, wav_path = tempfile.mkstemp(suffix='.wav', prefix='retrowp-dictate-')
        os.close(fd)
        cleanup = wav_path
        try:
            _write_wav(wav_path, pcm)
        except Exception:
            os.unlink(wav_path)
            return STATUS_ERR, b''

    try:
        text = _transcribe(cfg['cli'], cfg['model'], wav_path,
                           language, cfg['threads'])
    finally:
        if cleanup:
            try:
                os.unlink(cleanup)
            except OSError:
                pass

    if text is None:
        return STATUS_ERR, b''
    return STATUS_OK, text.encode('utf-8')


def handle_request(op, payload, cfg):
    if op == OP_DICTATE:
        return handle_dictate(payload, cfg)
    return STATUS_UNSUPPORTED, b''


def _probe_available(cli, model, audio_file, device):
    """Every dependency checked up front, so the CAPS bit tells the
    truth. Mirrors image_gateway.py's Pillow gate: a gateway that
    cannot hear must report OFFLOAD_DICTATE unsupported rather than
    accepting requests it will fail."""
    if not (os.path.isfile(cli) and os.access(cli, os.X_OK)):
        return False, 'whisper-cli not found or not executable: %s' % cli
    if not os.path.isfile(model):
        return False, 'model not found: %s' % model
    if audio_file:
        if not os.path.isfile(audio_file):
            return False, 'audio file not found: %s' % audio_file
        return True, 'ok (audio-file mode, no capture device needed)'
    if shutil.which('arecord') is None:
        return False, 'arecord not on PATH (alsa-utils not installed)'
    if device is None:
        return False, 'no ALSA capture device found (`arecord -l` lists none)'
    return True, 'ok (capture device %s)' % device


def main():
    args = parse_gateway_args(description=__doc__, extra_args=[
        (('--whisper-cli',), {
            'default': os.environ.get('RETROWP_WHISPER_CLI', DEFAULT_WHISPER_CLI),
            'help': 'path to whisper.cpp\'s whisper-cli binary',
        }),
        (('--whisper-model',), {
            'default': os.environ.get('RETROWP_WHISPER_MODEL', DEFAULT_WHISPER_MODEL),
            'help': 'path to a ggml whisper model (.bin)',
        }),
        (('--audio-device',), {
            'default': os.environ.get('RETROWP_AUDIO_DEVICE'),
            'help': 'ALSA capture device for arecord (default: first '
                     'capture card found via `arecord -l`)',
        }),
        (('--audio-file',), {
            'help': 'Test hook: transcribe this WAV instead of recording, '
                     'making the whole op deterministic and mic-free. Used '
                     'by host/tests to exercise the real transcription path '
                     'in CI, mirroring image_gateway.py\'s own test-hook '
                     'convention.',
        }),
        (('--threads',), {
            'type': int, 'default': os.cpu_count() or 4,
            'help': 'whisper.cpp worker threads',
        }),
        (('--force-whisper-unavailable',), {
            'action': 'store_true',
            'help': 'Test-only hook: report OFFLOAD_DICTATE as unsupported '
                     'regardless of whether whisper.cpp is actually present, '
                     'to exercise the degrade path deterministically.',
        }),
    ])

    device = args.audio_device or _detect_capture_device()
    available, why = _probe_available(args.whisper_cli, args.whisper_model,
                                       args.audio_file, device)
    if args.force_whisper_unavailable:
        available, why = False, 'forced unavailable (test hook)'
    print('dictate: %s' % why, file=sys.stderr, flush=True)

    cfg = {
        'available': available,
        'cli': args.whisper_cli,
        'model': args.whisper_model,
        'device': device,   # the RESOLVED device, never the raw argument:
                            # args.audio_device is None whenever auto-
                            # detection supplied it, and passing that
                            # through built ['arecord', '-D', None, ...]
        'audio_file': args.audio_file,
        'threads': args.threads,
    }
    ops_supported = (1 << OP_DICTATE) if available else 0
    return run_gateway_with_args(
        args, lambda op, payload: handle_request(op, payload, cfg),
        ops_supported, 'whisper-gateway')


if __name__ == '__main__':
    sys.exit(main() or 0)
