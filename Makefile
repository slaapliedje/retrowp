# Host build. engine/, offload/transport/, and services/ are portable C89
# and build under strict flags; platform/*, offload/gateway/, and host/*
# are host-only tooling and may use POSIX freely, under relaxed flags.
# See CLAUDE.md for the rules this enforces.

CC ?= cc

STRICT_CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Werror
RELAXED_CFLAGS = -Wall -Wextra

INCLUDES = -Iengine/include -Iplatform/host -Ioffload/transport -Ihost/tests

BUILD = build
BIN   = bin

ENGINE_SRC    = $(wildcard engine/src/*.c)
PLATFORM_SRC  = $(wildcard platform/host/*.c)
TRANSPORT_SRC = $(wildcard offload/transport/*.c)
TEST_SRC      = $(wildcard host/tests/*.c)
GATEWAY_SRC   = $(wildcard offload/gateway/*.c)

ENGINE_OBJ    = $(patsubst engine/src/%.c,$(BUILD)/engine/%.o,$(ENGINE_SRC))
PLATFORM_OBJ  = $(patsubst platform/host/%.c,$(BUILD)/platform/%.o,$(PLATFORM_SRC))
TRANSPORT_OBJ = $(patsubst offload/transport/%.c,$(BUILD)/transport/%.o,$(TRANSPORT_SRC))
TEST_OBJ      = $(patsubst host/tests/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))
GATEWAY_OBJ   = $(patsubst offload/gateway/%.c,$(BUILD)/gateway/%.o,$(GATEWAY_SRC))

# offload/transport (wire.c, caps_codec.c) uses engine/src/endian.c's
# be_read*/be_write* helpers. endian.c has no other engine dependency (no
# mem_alloc, no platform.h), so it's the one engine/src file the
# standalone gateway build links directly, without pulling in the rest of
# ENGINE_OBJ (which needs platform/host's mem_alloc -- unavailable here).
ENDIAN_OBJ    = $(BUILD)/engine/endian.o

.PHONY: all test clean

all: $(BIN)/host_tests $(BIN)/mock_gateway

$(BIN)/host_tests: $(ENGINE_OBJ) $(PLATFORM_OBJ) $(TRANSPORT_OBJ) $(TEST_OBJ) | $(BIN)
	$(CC) $^ -o $@

$(BIN)/mock_gateway: $(GATEWAY_OBJ) $(TRANSPORT_OBJ) $(ENDIAN_OBJ) | $(BIN)
	$(CC) $^ -o $@

$(BUILD)/engine/%.o: engine/src/%.c | $(BUILD)/engine
	$(CC) $(STRICT_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/platform/%.o: platform/host/%.c | $(BUILD)/platform
	$(CC) $(RELAXED_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/transport/%.o: offload/transport/%.c | $(BUILD)/transport
	$(CC) $(STRICT_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/tests/%.o: host/tests/%.c | $(BUILD)/tests
	$(CC) $(RELAXED_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/gateway/%.o: offload/gateway/%.c | $(BUILD)/gateway
	$(CC) $(RELAXED_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/engine $(BUILD)/platform $(BUILD)/transport $(BUILD)/tests $(BUILD)/gateway $(BIN):
	mkdir -p $@

test: $(BIN)/host_tests $(BIN)/mock_gateway
	./$(BIN)/host_tests

# ---------------------------------------------------------------------
# Atari ST cross build (M4). Real target toolchain, not a host stub:
# m68k-atari-mint-gcc defaults to -mcpu=68000 with no -mshort (32-bit
# int, matching types.h's u32/i32 == long -- see docs/atari-platform.md
# for why -mshort was deliberately rejected). platform/atari is UI-owning
# app code (CLAUDE.md: "platform/ owns UI + rendering"), so it links
# directly against libgem (AES+VDI) with no separate "backend" split the
# way platform/host has one.
# ---------------------------------------------------------------------

ATARICC ?= m68k-atari-mint-gcc

# -O2 is required, not just a size/speed nicety: at -O0 the smoke test's
# NF_STDERR/NF_EXIT Native Features trap (illegal-opcode inline asm,
# host/tools/atari_smoke.c) silently returns a null feature ID instead
# of trapping into Hatari, so pass/fail never reaches the emulator's own
# exit code -- reproduced and root-caused during T4.1 development, see
# docs/atari-platform.md.
ATARI_STRICT_CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Werror -O2
ATARI_RELAXED_CFLAGS = -Wall -Wextra -O2
ATARI_INCLUDES       = -Iengine/include -Iplatform/atari -Ioffload/transport

ATARI_BUILD = build/atari
ATARI_BIN   = bin/atari

ATARI_ENGINE_OBJ    = $(patsubst engine/src/%.c,$(ATARI_BUILD)/engine/%.o,$(ENGINE_SRC))
ATARI_TRANSPORT_OBJ = $(patsubst offload/transport/%.c,$(ATARI_BUILD)/transport/%.o,$(TRANSPORT_SRC))

# atari-engine-check is a separate, free portability check: it proves
# the SAME portable engine/ + offload/transport sources this project
# already tests on host also compile clean under strict C89 for a real
# 16/32-bit target -- catches host-only assumptions (pointer size,
# struct packing, etc.) independent of whether anything actually links
# against them yet.
.PHONY: atari-engine-check
atari-engine-check: $(ATARI_ENGINE_OBJ) $(ATARI_TRANSPORT_OBJ)
	@echo "engine + offload/transport cross-compile clean for m68k-atari-mint"

$(ATARI_BUILD)/engine/%.o: engine/src/%.c | $(ATARI_BUILD)/engine
	$(ATARICC) $(ATARI_STRICT_CFLAGS) $(ATARI_INCLUDES) -c $< -o $@

$(ATARI_BUILD)/transport/%.o: offload/transport/%.c | $(ATARI_BUILD)/transport
	$(ATARICC) $(ATARI_STRICT_CFLAGS) $(ATARI_INCLUDES) -c $< -o $@

# platform/atari's own sources, excluding the two files with their own
# main() (main.c for WP.PRG, and host/tools/atari_smoke.c for
# SMOKE.PRG) -- everything else (app shell, mem/metrics/draw/transport)
# is shared by both binaries.
ATARI_PLATFORM_SRC = $(filter-out platform/atari/main.c,$(wildcard platform/atari/*.c))
ATARI_PLATFORM_OBJ = $(patsubst platform/atari/%.c,$(ATARI_BUILD)/app/%.o,$(ATARI_PLATFORM_SRC))

$(ATARI_BUILD)/app/%.o: platform/atari/%.c | $(ATARI_BUILD)/app
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -c $< -o $@

$(ATARI_BIN)/WP.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/app/main.o $(ATARI_ENGINE_OBJ) $(ATARI_TRANSPORT_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

$(ATARI_BUILD)/smoke/%.o: host/tools/%.c | $(ATARI_BUILD)/smoke
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -c $< -o $@

$(ATARI_BIN)/SMOKE.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/smoke/atari_smoke.o $(ATARI_ENGINE_OBJ) $(ATARI_TRANSPORT_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

$(ATARI_BUILD)/engine $(ATARI_BUILD)/transport $(ATARI_BUILD)/app $(ATARI_BUILD)/smoke $(ATARI_BIN):
	mkdir -p $@

.PHONY: atari atari-smoke
atari: $(ATARI_BIN)/WP.PRG

atari-smoke: $(ATARI_BIN)/SMOKE.PRG

clean:
	rm -rf $(BUILD) $(BIN)
