# Host build. engine/, offload/transport/, and services/ are portable C89
# and build under strict flags; platform/*, offload/gateway/, and host/*
# are host-only tooling and may use POSIX freely, under relaxed flags.
# See CLAUDE.md for the rules this enforces.

CC ?= cc

STRICT_CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Werror
RELAXED_CFLAGS = -Wall -Wextra

INCLUDES = -Iengine/include -Iplatform/host -Iplatform/common -Ioffload/transport -Iservices/spell -Iservices/grammar -Ihost/tests -I$(GENERATED)

BUILD = build
BIN   = bin
GENERATED = build/generated

ENGINE_SRC    = $(wildcard engine/src/*.c)
PLATFORM_SRC  = $(wildcard platform/host/*.c)
# platform/common/ (M8.5): the action registry + app context shared by
# every backend -- see CLAUDE.md's platform/common/ layer rule. May call
# platform.h but must never #include a native header; built here under
# STRICT_CFLAGS (like ENGINE_OBJ) rather than PLATFORM_OBJ's RELAXED_CFLAGS,
# so a stray native #include breaks the host build immediately.
PLATFORM_COMMON_SRC = $(wildcard platform/common/*.c)
TRANSPORT_SRC = $(wildcard offload/transport/*.c)
SERVICES_SRC  = $(wildcard services/spell/*.c) $(wildcard services/grammar/*.c)
TEST_SRC      = $(wildcard host/tests/*.c)
GATEWAY_SRC   = $(wildcard offload/gateway/*.c)

ENGINE_OBJ    = $(patsubst engine/src/%.c,$(BUILD)/engine/%.o,$(ENGINE_SRC))
PLATFORM_OBJ  = $(patsubst platform/host/%.c,$(BUILD)/platform/%.o,$(PLATFORM_SRC))
PLATFORM_COMMON_OBJ = $(patsubst platform/common/%.c,$(BUILD)/platform_common/%.o,$(PLATFORM_COMMON_SRC))
TRANSPORT_OBJ = $(patsubst offload/transport/%.c,$(BUILD)/transport/%.o,$(TRANSPORT_SRC))
SERVICES_OBJ  = $(patsubst services/spell/%.c,$(BUILD)/services/%.o,$(patsubst services/grammar/%.c,$(BUILD)/services/%.o,$(SERVICES_SRC)))
TEST_OBJ      = $(patsubst host/tests/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))

# offload/gateway has TWO standalone executables (each with its own
# main()), sharing gw_common.c's wire-protocol plumbing -- so unlike
# every other *_OBJ above, this can't just be "every .c in the
# directory" linked as one unit; each binary below lists its own files
# explicitly.
MOCK_GATEWAY_OBJ   = $(BUILD)/gateway/mock_gateway.o $(BUILD)/gateway/fixtures.o $(BUILD)/gateway/gw_common.o
OLLAMA_GATEWAY_OBJ = $(BUILD)/gateway/ollama_gateway.o $(BUILD)/gateway/gw_common.o $(BUILD)/gateway/http_client.o

# offload/transport (wire.c, caps_codec.c) uses engine/src/endian.c's
# be_read*/be_write* helpers. endian.c has no other engine dependency (no
# mem_alloc, no platform.h), so it's the one engine/src file the
# standalone gateway build links directly, without pulling in the rest of
# ENGINE_OBJ (which needs platform/host's mem_alloc -- unavailable here).
ENDIAN_OBJ    = $(BUILD)/engine/endian.o

.PHONY: all test clean

all: $(BIN)/host_tests $(BIN)/mock_gateway $(BIN)/ollama_gateway

$(BIN)/host_tests: $(ENGINE_OBJ) $(PLATFORM_OBJ) $(PLATFORM_COMMON_OBJ) $(TRANSPORT_OBJ) $(SERVICES_OBJ) $(TEST_OBJ) | $(BIN)
	$(CC) $^ -o $@

$(BIN)/mock_gateway: $(MOCK_GATEWAY_OBJ) $(TRANSPORT_OBJ) $(ENDIAN_OBJ) | $(BIN)
	$(CC) $^ -o $@

# ollama_gateway (post-M6): answers OFFLOAD_THESAURUS with a real Ollama
# HTTP call (offload/gateway/http_client.c is a hand-rolled raw-socket
# HTTP/1.1 client -- no libcurl dependency) instead of mock_gateway's
# fixture table. See docs/formats/offload-protocol.md. Never part of
# `test`/`ci.sh`: LLM output isn't deterministic, and a contributor
# without Ollama installed must still be able to run the automated suite.
$(BIN)/ollama_gateway: $(OLLAMA_GATEWAY_OBJ) $(TRANSPORT_OBJ) $(ENDIAN_OBJ) | $(BIN)
	$(CC) $^ -o $@

$(BUILD)/engine/%.o: engine/src/%.c | $(BUILD)/engine
	$(CC) $(STRICT_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/platform/%.o: platform/host/%.c | $(BUILD)/platform
	$(CC) $(RELAXED_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/platform_common/%.o: platform/common/%.c | $(BUILD)/platform_common
	$(CC) $(STRICT_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/transport/%.o: offload/transport/%.c | $(BUILD)/transport
	$(CC) $(STRICT_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/services/%.o: services/spell/%.c | $(BUILD)/services
	$(CC) $(STRICT_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/services/%.o: services/grammar/%.c | $(BUILD)/services
	$(CC) $(STRICT_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/tests/%.o: host/tests/%.c | $(BUILD)/tests
	$(CC) $(RELAXED_CFLAGS) $(INCLUDES) -c $< -o $@

# test_help_content.c #includes the generated help header (M8.5 Phase
# 10) -- an explicit extra prerequisite, same reasoning as app_shell.o's
# own below.
$(BUILD)/tests/test_help_content.o: $(GENERATED)/help_retrowp.h

$(BUILD)/gateway/%.o: offload/gateway/%.c | $(BUILD)/gateway
	$(CC) $(RELAXED_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/tools/%.o: host/tools/%.c | $(BUILD)/tools
	$(CC) $(RELAXED_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/engine $(BUILD)/platform $(BUILD)/platform_common $(BUILD)/transport $(BUILD)/services $(BUILD)/tests $(BUILD)/gateway $(BUILD)/tools $(GENERATED) $(BIN):
	mkdir -p $@

# gen_help_header (M8.5 Phase 10) is a standalone host executable, not a
# .o linked into another binary (unlike everything else host/tools/%.o
# above) -- it runs AT BUILD TIME to turn docs/help/*.help source files
# into generated C headers (see the tool's own comment), so both the
# host test build and the Atari cross-compile can #include the result
# as ordinary source rather than hand-duplicating help content as an
# escaped C string someone has to remember to keep in sync.
$(BUILD)/gen_help_header: host/tools/gen_help_header.c | $(BUILD)
	$(CC) $(RELAXED_CFLAGS) $< -o $@

$(GENERATED)/help_retrowp.h: docs/help/retrowp.help $(BUILD)/gen_help_header | $(GENERATED)
	$(BUILD)/gen_help_header docs/help/retrowp.help $@ g_help_source_retrowp

test: $(BIN)/host_tests $(BIN)/mock_gateway
	./$(BIN)/host_tests

# Manual verification only (see ollama_gateway_smoke.c's own comment) --
# never a dependency of `all`/`test`.
$(BIN)/ollama_gateway_smoke: $(ENGINE_OBJ) $(PLATFORM_OBJ) $(TRANSPORT_OBJ) $(BUILD)/tools/ollama_gateway_smoke.o | $(BIN)
	$(CC) $^ -o $@

.PHONY: ollama-smoke
ollama-smoke: $(BIN)/ollama_gateway_smoke

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
ATARI_INCLUDES       = -Iengine/include -Iplatform/atari -Iplatform/common -Ioffload/transport -I$(GENERATED)

ATARI_BUILD = build/atari
ATARI_BIN   = bin/atari

ATARI_ENGINE_OBJ    = $(patsubst engine/src/%.c,$(ATARI_BUILD)/engine/%.o,$(ENGINE_SRC))
ATARI_TRANSPORT_OBJ = $(patsubst offload/transport/%.c,$(ATARI_BUILD)/transport/%.o,$(TRANSPORT_SRC))
ATARI_SERVICES_OBJ  = $(patsubst services/spell/%.c,$(ATARI_BUILD)/services/%.o,$(patsubst services/grammar/%.c,$(ATARI_BUILD)/services/%.o,$(SERVICES_SRC)))
ATARI_COMMON_OBJ    = $(patsubst platform/common/%.c,$(ATARI_BUILD)/common/%.o,$(PLATFORM_COMMON_SRC))

$(ATARI_BUILD)/common/%.o: platform/common/%.c | $(ATARI_BUILD)/common
	$(ATARICC) $(ATARI_STRICT_CFLAGS) $(ATARI_INCLUDES) -c $< -o $@

# atari-engine-check is a separate, free portability check: it proves
# the SAME portable engine/ + offload/transport + services/ sources this
# project already tests on host also compile clean under strict C89 for
# a real 16/32-bit target -- catches host-only assumptions (pointer
# size, struct packing, etc.) independent of whether anything actually
# links against them yet.
.PHONY: atari-engine-check
atari-engine-check: $(ATARI_ENGINE_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ)
	@echo "engine + offload/transport + services cross-compile clean for m68k-atari-mint"

$(ATARI_BUILD)/engine/%.o: engine/src/%.c | $(ATARI_BUILD)/engine
	$(ATARICC) $(ATARI_STRICT_CFLAGS) $(ATARI_INCLUDES) -c $< -o $@

$(ATARI_BUILD)/transport/%.o: offload/transport/%.c | $(ATARI_BUILD)/transport
	$(ATARICC) $(ATARI_STRICT_CFLAGS) $(ATARI_INCLUDES) -c $< -o $@

$(ATARI_BUILD)/services/%.o: services/spell/%.c | $(ATARI_BUILD)/services
	$(ATARICC) $(ATARI_STRICT_CFLAGS) $(ATARI_INCLUDES) -c $< -o $@

$(ATARI_BUILD)/services/%.o: services/grammar/%.c | $(ATARI_BUILD)/services
	$(ATARICC) $(ATARI_STRICT_CFLAGS) $(ATARI_INCLUDES) -c $< -o $@

# platform/atari's own sources, excluding the two files with their own
# main() (main.c for RETROWP.PRG, and host/tools/atari_smoke.c for
# SMOKE.PRG) -- everything else (app shell, mem/metrics/draw/transport)
# is shared by both binaries.
ATARI_PLATFORM_SRC = $(filter-out platform/atari/main.c,$(wildcard platform/atari/*.c))
ATARI_PLATFORM_OBJ = $(patsubst platform/atari/%.c,$(ATARI_BUILD)/app/%.o,$(ATARI_PLATFORM_SRC))

$(ATARI_BUILD)/app/%.o: platform/atari/%.c | $(ATARI_BUILD)/app
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -Iplatform/common -Iservices/spell -Iservices/grammar -I$(GENERATED) -c $< -o $@

# app_shell.c #includes the generated help header (M8.5 Phase 10) --
# an explicit extra prerequisite since Make can't see into #include
# lines on its own the way it tracks the rule graph above.
$(ATARI_BUILD)/app/app_shell.o: $(GENERATED)/help_retrowp.h

$(ATARI_BIN)/RETROWP.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/app/main.o $(ATARI_ENGINE_OBJ) $(ATARI_COMMON_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

$(ATARI_BUILD)/smoke/%.o: host/tools/%.c | $(ATARI_BUILD)/smoke
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -c $< -o $@

$(ATARI_BIN)/SMOKE.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/smoke/atari_smoke.o $(ATARI_ENGINE_OBJ) $(ATARI_COMMON_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

# T6.3's serial offload smoke test doesn't touch any AES/window API
# itself, but ATARI_ENGINE_OBJ pulls in the whole engine (docmodel.c's
# cleanup path reaches linebreak.c regardless of whether this test ever
# lays out text), which needs metrics_atari.c/draw_atari.c's VDI calls
# -- so this still needs the full platform/atari object set and -lgem,
# same as RETROWP.PRG/SMOKE.PRG. Only the source file differs.
$(ATARI_BUILD)/serialsmoke/%.o: host/tools/%.c | $(ATARI_BUILD)/serialsmoke
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -Ioffload/transport -c $< -o $@

$(ATARI_BIN)/SERIALTST.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/serialsmoke/atari_serial_smoke.o $(ATARI_ENGINE_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

# M7 T7.4c: same shape as SERIALTST.PRG, proving CONVERT_IMPORT/EXPORT
# (not just THESAURUS) survive real RS-232 -- see atari_convert_smoke.c.
$(ATARI_BUILD)/convertsmoke/%.o: host/tools/%.c | $(ATARI_BUILD)/convertsmoke
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -Ioffload/transport -c $< -o $@

$(ATARI_BIN)/CONVERTST.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/convertsmoke/atari_convert_smoke.o $(ATARI_ENGINE_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

$(ATARI_BUILD)/engine $(ATARI_BUILD)/transport $(ATARI_BUILD)/services $(ATARI_BUILD)/common $(ATARI_BUILD)/app $(ATARI_BUILD)/smoke $(ATARI_BUILD)/serialsmoke $(ATARI_BUILD)/convertsmoke $(ATARI_BIN):
	mkdir -p $@

.PHONY: atari atari-smoke atari-serial-smoke atari-convert-smoke
atari: $(ATARI_BIN)/RETROWP.PRG

atari-smoke: $(ATARI_BIN)/SMOKE.PRG

atari-serial-smoke: $(ATARI_BIN)/SERIALTST.PRG

atari-convert-smoke: $(ATARI_BIN)/CONVERTST.PRG

clean:
	rm -rf $(BUILD) $(BIN)
