#!/bin/sh
# Manual verification for the Ollama-backed gateway (post-M6, see
# offload/gateway/ollama_gateway.c and docs/formats/offload-protocol.md).
# NOT part of ci.sh/make test -- requires a real Ollama server reachable
# at --ollama-host/--ollama-port (127.0.0.1:11434 by default) and
# produces non-deterministic output, neither of which belongs in the
# automated suite. Run this by hand to prove the real thing works.
#
# Usage: host/tools/run_ollama_smoke.sh [word] [paragraph] [-- <extra ollama_gateway args>]
set -e

cd "$(dirname "$0")/../.."

WORD="${1:-happy}"
PARAGRAPH="${2:-}"

make bin/ollama_gateway ollama-smoke >&2

PORT_FILE="$(mktemp)"
# `kill` on an already-exited GW_PID (the gateway's single-connection
# loop exits on its own once ollama_gateway_smoke disconnects) returns
# nonzero -- under `set -e`, an EXIT trap that fails partway through
# both skips its own remaining cleanup (rm -f never ran) AND clobbers
# the script's real exit status with the trap's failure, silently
# turning a fully-passing run into a reported failure. `|| true` makes
# that failure non-fatal so the trap always finishes and the exit
# status the last real command set (ollama_gateway_smoke's) survives.
trap 'kill "$GW_PID" 2>/dev/null || true; rm -f "$PORT_FILE"' EXIT

./bin/ollama_gateway > "$PORT_FILE" &
GW_PID=$!

# A bare `[ -n "$PORT" ] && break` inside this loop would hit the same
# set -e trap: on any iteration where PORT is still empty, that
# AND-list's exit status is nonzero and isn't a while/if condition, so
# errexit would abort the whole script on the very first empty check
# instead of retrying. Use an explicit while-condition (exempt from
# errexit) instead.
PORT=""
i=0
while [ -z "$PORT" ] && [ "$i" -lt 50 ]; do
    PORT="$(cat "$PORT_FILE" 2>/dev/null || true)"
    if [ -z "$PORT" ]; then
        sleep 0.1
    fi
    i=$((i + 1))
done

if [ -z "$PORT" ]; then
    echo "ollama_gateway never printed a port -- see stderr above" >&2
    exit 1
fi

if [ -n "$PARAGRAPH" ]; then
    ./bin/ollama_gateway_smoke "tcp:127.0.0.1:$PORT" "$WORD" "$PARAGRAPH"
else
    ./bin/ollama_gateway_smoke "tcp:127.0.0.1:$PORT" "$WORD"
fi
