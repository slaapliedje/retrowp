# Host build. engine/, offload/transport/, and services/ are portable C89
# and build under strict flags; platform/*, offload/gateway/, and host/*
# are host-only tooling and may use POSIX freely, under relaxed flags.
# See CLAUDE.md for the rules this enforces.

CC ?= cc

# Header-dependency tracking. Without this, `make` rebuilds an object only
# when its own .c changes -- never when a header it includes changes -- so
# editing a shared header silently leaves a mix of objects compiled against
# the old and new versions of it. That is not a theoretical tidiness point:
# it produced two CONFUSING FALSE FAILURES in a single session, each of
# which looked like a real defect in correct code.
#
#   - Adding two enum values to platform/common/actions.h left
#     menucheck.o holding the old WP_ACT_COUNT while actions.o had the
#     new one, so a bounds check compiled to a comparison against 17
#     while the table had 19 rows -- a host test failed on correct code.
#   - Renumbering WP_MENU_OBJ_* left atari_smoke.o sending the old index
#     for FILE_QUIT while app_shell.o's dispatch table held the new one.
#     ALL SIX TOS IMAGES plus the AFROS gate went red against a
#     completely correct menu tree.
#
# A gate that reds on correct code is worse than a slow build: it teaches
# you to distrust the one thing that catches real regressions.
#
# -MMD writes a .d beside each .o listing the headers that object really
# read (-MMD not -MD: system headers are excluded, they don't change here).
# -MP adds a phony target for each of those headers, so DELETING a header
# doesn't wedge make with "No rule to make target" -- it just rebuilds.
DEPFLAGS = -MMD -MP


STRICT_CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Werror $(DEPFLAGS)
RELAXED_CFLAGS = -Wall -Wextra $(DEPFLAGS)

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

# gen_help_guide (M9 Phase 6): a real AmigaGuide (.guide) exporter, a
# genuine deliverable (not a spike-and-defer the way ST Guide's .HYP
# stayed, docs/help.md) since AmigaGuide needs no external compiler --
# unlike gen_help_header above, this links against the real portable
# parser (help_doc.o + strpool.o + the host stub's mem_host.o for
# mem_alloc/mem_free) instead of just copying bytes, so it walks the
# same node/line graph app_shell.c's own pager does rather than
# re-deriving the format by hand. See the tool's own file comment.
$(BUILD)/gen_help_guide: host/tools/gen_help_guide.c $(BUILD)/platform_common/help_doc.o $(BUILD)/engine/strpool.o $(BUILD)/platform/mem_host.o | $(BUILD)
	$(CC) $(RELAXED_CFLAGS) $(INCLUDES) $^ -o $@

$(GENERATED)/retrowp.guide: docs/help/retrowp.help $(BUILD)/gen_help_guide | $(GENERATED)
	$(BUILD)/gen_help_guide docs/help/retrowp.help $@ "RetroWP Help"

# Printable keyboard reference cards, one per keybinding scheme -- the
# strip that used to sit above the function keys. Generated, never hand
# written, from the two files the app itself is built from: docs/
# keymaps.md's own reference tables and platform/common/actions.c's
# g_wp_actions[]. A hand-typed card is wrong the first time somebody
# adds a binding, and a card you cannot trust is worse than none.
#
# Python, unlike gen_help_header/gen_help_guide above. Those are C so
# they can link the real engine parsers; there is no engine parser for
# Markdown, so that reason does not apply here.
.PHONY: keycards
keycards: $(GENERATED)/keycards/index.html

$(GENERATED)/keycards/index.html: host/tools/gen_keycard.py docs/keymaps.md platform/common/actions.c | $(GENERATED)
	python3 host/tools/gen_keycard.py . $(GENERATED)/keycards

.PHONY: help-guide
help-guide: $(GENERATED)/retrowp.guide

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
ATARI_STRICT_CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Werror -O2 $(DEPFLAGS)
ATARI_RELAXED_CFLAGS = -Wall -Wextra -O2 $(DEPFLAGS)
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
#
# ATARI_COMMON_OBJ is required for the same reason, and its absence here
# was a real, silent rot: ATARI_PLATFORM_OBJ contains app_shell.o, which
# has referenced platform/common symbols (help viewer, keymap, config,
# app context) since M8.5 added them -- so this rule and CONVERTST.PRG's
# below stopped linking then and stayed broken, taking ci_atari_serial.sh
# and ci_atari_convert_serial.sh down with them (both `make` these
# targets first, under `set -e`). RETROWP.PRG and SMOKE.PRG list it and
# were never affected. Any future .PRG linking ATARI_PLATFORM_OBJ needs
# ATARI_COMMON_OBJ too.
$(ATARI_BUILD)/serialsmoke/%.o: host/tools/%.c | $(ATARI_BUILD)/serialsmoke
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -Ioffload/transport -c $< -o $@

$(ATARI_BIN)/SERIALTST.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/serialsmoke/atari_serial_smoke.o $(ATARI_ENGINE_OBJ) $(ATARI_COMMON_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

# M7 T7.4c: same shape as SERIALTST.PRG, proving CONVERT_IMPORT/EXPORT
# (not just THESAURUS) survive real RS-232 -- see atari_convert_smoke.c.
$(ATARI_BUILD)/convertsmoke/%.o: host/tools/%.c | $(ATARI_BUILD)/convertsmoke
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -Ioffload/transport -c $< -o $@

$(ATARI_BIN)/CONVERTST.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/convertsmoke/atari_convert_smoke.o $(ATARI_ENGINE_OBJ) $(ATARI_COMMON_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

# The multitasking-AES redraw prober (host/tools/atari_wmredraw_probe.c).
# Unlike every other .PRG here it links NOTHING but gemlib -- it is a
# second AES process that covers and uncovers RetroWP's window, and has
# no business knowing what a document is. Keeping it engine-free also
# keeps it buildable and startable in a fraction of RETROWP.PRG's time.
$(ATARI_BUILD)/redrawprobe/%.o: host/tools/%.c | $(ATARI_BUILD)/redrawprobe
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -c $< -o $@

$(ATARI_BIN)/WMREDRAW.PRG: $(ATARI_BUILD)/redrawprobe/atari_wmredraw_probe.o | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

$(ATARI_BUILD)/engine $(ATARI_BUILD)/transport $(ATARI_BUILD)/services $(ATARI_BUILD)/common $(ATARI_BUILD)/app $(ATARI_BUILD)/smoke $(ATARI_BUILD)/serialsmoke $(ATARI_BUILD)/serialhw $(ATARI_BUILD)/convertsmoke $(ATARI_BUILD)/redrawprobe $(ATARI_BIN):
	mkdir -p $@

.PHONY: atari atari-smoke atari-serial-smoke atari-serial-hw atari-serial-raw atari-serial-mint atari-convert-smoke atari-wmredraw-probe
atari: $(ATARI_BIN)/RETROWP.PRG

atari-smoke: $(ATARI_BIN)/SMOKE.PRG

# SERHW.PRG: the REAL-HARDWARE serial diagnostic. Same object set as
# SERIALTST.PRG, but its source uses no Native Features at all -- see
# host/tools/atari_serial_hw.c for why that distinction is load-bearing
# (0x7300 is an illegal instruction on a real 68030, so every other
# smoke binary in this project would crash the machine on launch).
# Not part of any CI gate: it needs a real cable and a real gateway,
# which is exactly the thing CI cannot supply.
$(ATARI_BUILD)/serialhw/%.o: host/tools/%.c | $(ATARI_BUILD)/serialhw
	$(ATARICC) $(ATARI_RELAXED_CFLAGS) -Iengine/include -Iplatform/atari -Ioffload/transport -c $< -o $@

$(ATARI_BIN)/SERHW.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/serialhw/atari_serial_hw.o $(ATARI_ENGINE_OBJ) $(ATARI_COMMON_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

atari-serial-hw: $(ATARI_BIN)/SERHW.PRG

# SERRAW.PRG: protocol-free bytes-only test, for when SERHW says nothing
# handshook and the question becomes WHICH direction is dead.
$(ATARI_BIN)/SERRAW.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/serialhw/atari_serial_raw.o $(ATARI_ENGINE_OBJ) $(ATARI_COMMON_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

atari-serial-raw: $(ATARI_BIN)/SERRAW.PRG

# SERMINT.PRG: reads the handshake lines from the ATARI's own side via
# MiNT's TIOCMGET, for when SERRAW has narrowed the fault to "transmit
# is gated" but cannot say whether CTS is actually arriving. Answers
# with software what would otherwise need a multimeter inside an open
# machine. MiNT-only by construction -- it says so and exits cleanly
# when the ioctl is unavailable.
$(ATARI_BIN)/SERMINT.PRG: $(ATARI_PLATFORM_OBJ) $(ATARI_BUILD)/serialhw/atari_serial_mint.o $(ATARI_ENGINE_OBJ) $(ATARI_COMMON_OBJ) $(ATARI_TRANSPORT_OBJ) $(ATARI_SERVICES_OBJ) | $(ATARI_BIN)
	$(ATARICC) $^ -o $@ -lgem

atari-serial-mint: $(ATARI_BIN)/SERMINT.PRG

atari-serial-smoke: $(ATARI_BIN)/SERIALTST.PRG

atari-convert-smoke: $(ATARI_BIN)/CONVERTST.PRG

atari-wmredraw-probe: $(ATARI_BIN)/WMREDRAW.PRG

# ---------------------------------------------------------------------
# Amiga cross build (M9). Real target toolchain, not a host stub: Bebbo's
# m68k-amigaos-gcc (github.com/amiga-mui/muidev-adjacent NDK/SDK tooling
# lives under the same toolchain; see docs/amiga-platform.md), built from
# source into ~/opt/amiga (not a stock package -- see
# docs/toolchain-amiga-notes.md if present, or the upstream project's own
# docs/toolchain-amiga.md this recipe was adapted from).
#
# CPU tier is a deliberate baseline choice, not a codegen accident: this
# project's Amiga floor is a stock machine (512KB-1MB chip RAM, no
# accelerator), matching the Atari ST backend's own "widest real-hardware
# floor" baseline -- so -m68000 -msoft-float, not -m68020, is the default
# here (a different, unrelated project on this machine that also uses
# this exact toolchain targets AGA/68020 as ITS baseline; that choice
# doesn't apply to RetroWP).
#
# -fbbb=abcefilmnprsz0 (default minus "h") works around a real GCC 6.5
# miscompiler bug in Bebbo's toolchain: the "optimize shift instructions"
# pass can narrow a (long)short*1024-shaped shift to HImode across an
# intervening call and silently drop the carry-out (confirmed via a
# reproducer on this same toolchain by a different project before this
# backend existed) -- included defensively from the start rather than
# waiting to rediscover it.
# ---------------------------------------------------------------------

AMIGA_TOOLROOT ?= $(HOME)/opt/amiga
AMIGACC        ?= $(AMIGA_TOOLROOT)/bin/m68k-amigaos-gcc

AMIGA_CPU = -m68000 -msoft-float -fbbb=abcefilmnprsz0

AMIGA_STRICT_CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Werror $(AMIGA_CPU) $(DEPFLAGS)
AMIGA_RELAXED_CFLAGS = -Wall -Wextra $(AMIGA_CPU) $(DEPFLAGS)
AMIGA_INCLUDES       = -Iengine/include -Iplatform/amiga -Iplatform/common -Ioffload/transport -I$(GENERATED)

AMIGA_BUILD = build/amiga
AMIGA_BIN   = bin/amiga

AMIGA_ENGINE_OBJ    = $(patsubst engine/src/%.c,$(AMIGA_BUILD)/engine/%.o,$(ENGINE_SRC))
AMIGA_TRANSPORT_OBJ = $(patsubst offload/transport/%.c,$(AMIGA_BUILD)/transport/%.o,$(TRANSPORT_SRC))
AMIGA_SERVICES_OBJ  = $(patsubst services/spell/%.c,$(AMIGA_BUILD)/services/%.o,$(patsubst services/grammar/%.c,$(AMIGA_BUILD)/services/%.o,$(SERVICES_SRC)))
AMIGA_COMMON_OBJ    = $(patsubst platform/common/%.c,$(AMIGA_BUILD)/common/%.o,$(PLATFORM_COMMON_SRC))

$(AMIGA_BUILD)/common/%.o: platform/common/%.c | $(AMIGA_BUILD)/common
	$(AMIGACC) $(AMIGA_STRICT_CFLAGS) $(AMIGA_INCLUDES) -c $< -o $@

# amiga-engine-check mirrors atari-engine-check exactly: a link-free
# portability gate proving the same portable engine/ + offload/transport
# + services/ sources already tested on host also compile clean under
# strict C89 for this second real target -- independent of whether
# platform/amiga/ has any real platform.h implementation yet.
.PHONY: amiga-engine-check
amiga-engine-check: $(AMIGA_ENGINE_OBJ) $(AMIGA_TRANSPORT_OBJ) $(AMIGA_SERVICES_OBJ)
	@echo "engine + offload/transport + services cross-compile clean for m68k-amigaos"

$(AMIGA_BUILD)/engine/%.o: engine/src/%.c | $(AMIGA_BUILD)/engine
	$(AMIGACC) $(AMIGA_STRICT_CFLAGS) $(AMIGA_INCLUDES) -c $< -o $@

$(AMIGA_BUILD)/transport/%.o: offload/transport/%.c | $(AMIGA_BUILD)/transport
	$(AMIGACC) $(AMIGA_STRICT_CFLAGS) $(AMIGA_INCLUDES) -c $< -o $@

$(AMIGA_BUILD)/services/%.o: services/spell/%.c | $(AMIGA_BUILD)/services
	$(AMIGACC) $(AMIGA_STRICT_CFLAGS) $(AMIGA_INCLUDES) -c $< -o $@

$(AMIGA_BUILD)/services/%.o: services/grammar/%.c | $(AMIGA_BUILD)/services
	$(AMIGACC) $(AMIGA_STRICT_CFLAGS) $(AMIGA_INCLUDES) -c $< -o $@

# platform/amiga/'s own sources (app shell, mem/metrics/draw/transport)
# -- empty today (M9 Phase 0), populated starting Phase 1. Mirrors
# ATARI_PLATFORM_SRC/_OBJ's shape exactly, main.c excluded once it exists.
AMIGA_PLATFORM_SRC = $(filter-out platform/amiga/main.c,$(wildcard platform/amiga/*.c))
AMIGA_PLATFORM_OBJ = $(patsubst platform/amiga/%.c,$(AMIGA_BUILD)/app/%.o,$(AMIGA_PLATFORM_SRC))

$(AMIGA_BUILD)/app/%.o: platform/amiga/%.c | $(AMIGA_BUILD)/app
	$(AMIGACC) $(AMIGA_RELAXED_CFLAGS) -Iengine/include -Iplatform/amiga -Iplatform/common -Iservices/spell -Iservices/grammar -I$(GENERATED) -c $< -o $@

# T9.1a smoke test (memory/file/clipboard/timer only -- no MUI/Intuition
# yet). -noixemul is a LINK-time-only flag (selects libnix's standalone
# crt0/libc instead of ixemul) -- passing it to a *compile* step instead
# flips newlib's reent header path and breaks <stdio.h> for anything that
# uses it (a real gotcha hit by the unrelated OpenUA project on this same
# toolchain; see docs/amiga-platform.md), so it appears only on the final
# link line below, never in AMIGA_STRICT_CFLAGS/AMIGA_RELAXED_CFLAGS.
$(AMIGA_BUILD)/smoke/%.o: host/tools/%.c | $(AMIGA_BUILD)/smoke
	$(AMIGACC) $(AMIGA_RELAXED_CFLAGS) -Iengine/include -Iplatform/amiga -c $< -o $@

# T9.1b's MUI shell and T9.1a's selfcheck live in the same app_shell.c
# (mirroring platform/atari/app_shell.c's own single-big-file precedent),
# so both AMISMOKE and RETROWP need the same broad link set below --
# splitting the smoke test's link line narrower stopped being possible
# once app_shell.c grew past the non-UI seams alone. linebreak.o IS
# linked: wp_document_free/edit.c call wp_paragraph_layout_free
# unconditionally (also defined in linebreak.c), so metrics_amiga.c
# supplies real-but-unreachable-today stubs for the plat_font_*
# functions linebreak.o's OWN wp_paragraph_relayout references, rather
# than excluding the whole object (see metrics_amiga.c's own comment).
# AMIGA_COMMON_OBJ now links unfiltered too (a prior revision excluded
# help_doc.o/help_view.o/statusline.o: they need plat_draw_text/
# plat_fill_rect/plat_gc_flush, which metrics_amiga.c didn't stub before
# T9.2 landed). Phase 6 wires the real help viewer into app_shell.c
# (do_help_contents/amiga_dispatch_key/editor_draw all reference it
# now), so that filter-out became a no-op and was dropped, the same way
# T9.3 dropped AMIGA_ENGINE_OBJ's own now-pointless one below --
# AMIGA_APP_COMMON_OBJ was this variable's name until this revision;
# removed as dead indirection once its filter had nothing left to do.
#
# T9.3: AMIGA_ENGINE_OBJ now links unfiltered -- offload_client.o
# (plat_transport_*/wire_*/caps_* -- previously excluded, since nothing
# called it) is genuinely used now (ensure_offload()/do_thesaurus()/
# do_chat()/do_grammar_check()), so the previous AMIGA_APP_ENGINE_OBJ
# filter-out became a no-op filter and was removed rather than kept as
# dead indirection. AMIGA_TRANSPORT_OBJ (offload_client.o's own
# wire.o/caps_codec.o dependencies) and AMIGA_SERVICES_OBJ (grammar.o,
# for do_grammar_check's local grammar_lite_check() fallback when no
# offload device is attached -- CLAUDE.md's Offload Rules; spell.o rides
# along unused, since AMIGA_SERVICES_OBJ bundles both -- see
# app_shell.c's own comment on why Spellcheck itself stays a no-op) are
# both added to the link for the same reason.

# Phase 6: app_shell.c #includes the generated help_retrowp.h (shared
# verbatim with platform/atari/app_shell.c's own dependency line below).
$(AMIGA_BUILD)/app/app_shell.o: $(GENERATED)/help_retrowp.h

$(AMIGA_BIN)/AMISMOKE: $(AMIGA_PLATFORM_OBJ) $(AMIGA_BUILD)/smoke/amiga_smoke.o $(AMIGA_COMMON_OBJ) $(AMIGA_ENGINE_OBJ) $(AMIGA_TRANSPORT_OBJ) $(AMIGA_SERVICES_OBJ) | $(AMIGA_BIN)
	$(AMIGACC) -noixemul $^ -o $@ -lmui

$(AMIGA_BIN)/RETROWP: $(AMIGA_PLATFORM_OBJ) $(AMIGA_BUILD)/app/main.o $(AMIGA_COMMON_OBJ) $(AMIGA_ENGINE_OBJ) $(AMIGA_TRANSPORT_OBJ) $(AMIGA_SERVICES_OBJ) | $(AMIGA_BIN)
	$(AMIGACC) -noixemul $^ -o $@ -lmui

.PHONY: amiga-app
amiga-app: $(AMIGA_BIN)/RETROWP

$(AMIGA_BUILD)/engine $(AMIGA_BUILD)/transport $(AMIGA_BUILD)/services $(AMIGA_BUILD)/common $(AMIGA_BUILD)/app $(AMIGA_BUILD)/smoke $(AMIGA_BIN):
	mkdir -p $@

.PHONY: amiga-smoke
amiga-smoke: $(AMIGA_BIN)/AMISMOKE

# ---------------------------------------------------------------------
# Mac Classic (M12) -- platform/mac uses Retro68 (m68k-apple-macos-gcc)
# via host/tools/mac/CMakeLists.txt for actual app/smoke-test builds:
# Mac resource-fork packaging needs Retro68's own Rez-based CMake macro
# (add_application()), not a plain compile+link recipe -- see
# docs/mac-platform.md. mac-engine-check mirrors atari-engine-check/
# amiga-engine-check exactly: a link-free portability check of the SAME
# portable engine/ + offload/transport + services/ sources, independent
# of CMake -- MAC_BUILD is deliberately its own directory, separate from
# build/mac (host/tools/ci_mac.sh's own CMake build tree).
# ---------------------------------------------------------------------

MAC_TOOLROOT ?= $(HOME)/opt/Retro68-build/toolchain
MACCC        ?= $(MAC_TOOLROOT)/bin/m68k-apple-macos-gcc

MAC_STRICT_CFLAGS = -std=c89 -pedantic -Wall -Wextra -Werror -O2 $(DEPFLAGS)
MAC_INCLUDES      = -Iengine/include -Iplatform/mac -Iplatform/common -Ioffload/transport -I$(GENERATED)

MAC_BUILD = build/mac_engine_check

MAC_ENGINE_OBJ    = $(patsubst engine/src/%.c,$(MAC_BUILD)/engine/%.o,$(ENGINE_SRC))
MAC_TRANSPORT_OBJ = $(patsubst offload/transport/%.c,$(MAC_BUILD)/transport/%.o,$(TRANSPORT_SRC))
MAC_SERVICES_OBJ  = $(patsubst services/spell/%.c,$(MAC_BUILD)/services/%.o,$(patsubst services/grammar/%.c,$(MAC_BUILD)/services/%.o,$(SERVICES_SRC)))

.PHONY: mac-engine-check
mac-engine-check: $(MAC_ENGINE_OBJ) $(MAC_TRANSPORT_OBJ) $(MAC_SERVICES_OBJ)
	@echo "engine + offload/transport + services cross-compile clean for m68k-apple-macos"

$(MAC_BUILD)/engine/%.o: engine/src/%.c | $(MAC_BUILD)/engine
	$(MACCC) $(MAC_STRICT_CFLAGS) $(MAC_INCLUDES) -c $< -o $@

$(MAC_BUILD)/transport/%.o: offload/transport/%.c | $(MAC_BUILD)/transport
	$(MACCC) $(MAC_STRICT_CFLAGS) $(MAC_INCLUDES) -c $< -o $@

$(MAC_BUILD)/services/%.o: services/spell/%.c | $(MAC_BUILD)/services
	$(MACCC) $(MAC_STRICT_CFLAGS) $(MAC_INCLUDES) -c $< -o $@

$(MAC_BUILD)/services/%.o: services/grammar/%.c | $(MAC_BUILD)/services
	$(MACCC) $(MAC_STRICT_CFLAGS) $(MAC_INCLUDES) -c $< -o $@

$(MAC_BUILD)/engine $(MAC_BUILD)/transport $(MAC_BUILD)/services:
	mkdir -p $@

# ---------------------------------------------------------------------
# DOS (M14) -- Open Watcom v2 (AUR: openwatcom-v2), a real, actively
# maintained cross-compiler producing genuine 16-bit real-mode DOS
# .EXE files from Linux (-bt=dos), the same "modern cross-compiler,
# real vintage target" shape m68k-amigaos-gcc/Retro68 already
# established, not a from-scratch build (unlike Retro68) -- installed
# here as a user-local prefix (DOS_TOOLROOT), matching AMIGA_TOOLROOT's
# own convention, rather than a system pacman package. Unlike
# gcc-family cross-compilers, Watcom's compile (wcc) and link (wcl)
# steps are separate tools with their own flag dialect (-bt=dos target
# OS, -za -zastd=c89 strict ISO C89, -we -wx warnings-as-errors at max
# level, -0 8086 baseline matching this project's widest-compatibility
# ethos); wcl additionally needs WATCOM set and wlink on PATH to locate
# its own runtime libraries at link time -- confirmed directly, not
# assumed (a clean-environment link fails with "cannot open clibs.lib"
# otherwise). DOS_BUILD is deliberately its own tree, mirroring every
# other cross-target's own *_BUILD separation.
#
# Memory model is -ml (large: far/multi-segment code AND far/multi-
# segment data). The progression here was forced, one step at a time,
# never chosen for headroom:
#   -ms (small) overflowed the one 64KB code segment by ~16KB at the
#       very first real link of the full engine (every format importer/
#       exporter, all five keybinding schemes).
#   -mm (medium) fixed that with far calls, and held until the DATA side
#       ran out too. Medium keeps all static data, the heap and the stack
#       inside a single 64KB DGROUP -- and once RETROWP.EXE also carried
#       the generated help document, only ~8KB of heap was left. That is
#       smaller than linebreak.c's 14KB token buffer (512 tokens), which
#       moved from stack to heap when the Amiga's 4KB stack forced it
#       there. Every wp_paragraph_relayout call then returned WP_NOMEM,
#       so every paragraph laid out to ZERO lines and the app painted a
#       blank page while looking perfectly healthy otherwise (T171).
#   -ml (large) puts data in far, multi-segment memory, so the heap draws
#       on conventional memory instead of the DGROUP leftovers.
# This is exactly the "unless something concrete forces it" escalation
# this comment used to reserve, and the token buffer is what forced it.
# The engine makes no pointer-size assumptions (a CLAUDE.md rule), which
# is what makes the switch a build-flag change rather than a port.
# ---------------------------------------------------------------------

DOS_TOOLROOT ?= $(HOME)/opt/watcom
DOSCC        ?= $(DOS_TOOLROOT)/binl64/wcc
DOSLINK      ?= $(DOS_TOOLROOT)/binl64/wcl
DOS_ENV       = WATCOM=$(DOS_TOOLROOT) PATH=$(DOS_TOOLROOT)/binl64:$$PATH

# Watcom's own make-dependency generation. Not $(DEPFLAGS): wcc names the
# dependency file explicitly rather than deriving it from the object, so
# these have to be expanded per-rule where $@ exists. -adfs forces '/'
# separators (wcc defaults to DOS-style '\', which GNU make would read as
# line continuations); -adt names the target, which would otherwise be
# written without the build/dos/... prefix and match nothing; -add names
# the FIRST dependency, which wcc otherwise writes as a bare basename
# ("app_shell.c", not "platform/dos/app_shell.c") that make cannot
# resolve -- found by running make twice, since the .d files are only
# consumed on the invocation AFTER the one that writes them.
#
# wcc has no -MP equivalent, so dos_depfix below synthesises the same
# phony header targets afterwards. That is not optional politeness: the
# -include at the bottom of this file globs EVERY .d under build/, so a
# .d without phony targets wedges the HOST build too, not just DOS --
# "No rule to make target 'platform/common/menucheck.h'" from `make
# test`, which would be a thoroughly baffling way to learn that a DOS
# object file remembered a header you deleted.
DOS_DEPFLAGS = -ad=$(@:.o=.d) -adfs -adt=$@ -add=$<

# Appends "<header>:" phony targets to a Watcom-generated .d, matching
# what GCC's -MP emits inline. Takes the .d path as $(1).
define dos_depfix
tr ' ' '\n' < $(1) | sed -n 's/^\(.*\.h\)$$/\1:/p' >> $(1)
endef

DOS_STRICT_CFLAGS  = -0 -ml -bt=dos -za -zastd=c89 -we -wx
DOS_RELAXED_CFLAGS = -0 -ml -bt=dos -wx
# engine/include first, deliberately: Watcom's own h/ ships a same-
# named endian.h (OS/2-oriented) that shadowed this project's real
# engine/include/endian.h when searched first, producing bizarre
# parse errors deep in Watcom's header, not this project's -- found
# directly during Phase 0 bring-up, not assumed. Project headers must
# win over Watcom's system headers on any name collision.
DOS_INCLUDES       = -i=engine/include -i=platform/dos -i=platform/common -i=offload/transport -i=$(GENERATED) -i=$(DOS_TOOLROOT)/h

DOS_BUILD = build/dos
DOS_BIN   = bin/dos

DOS_ENGINE_OBJ    = $(patsubst engine/src/%.c,$(DOS_BUILD)/engine/%.o,$(ENGINE_SRC))
DOS_TRANSPORT_OBJ = $(patsubst offload/transport/%.c,$(DOS_BUILD)/transport/%.o,$(TRANSPORT_SRC))
DOS_SERVICES_OBJ  = $(patsubst services/spell/%.c,$(DOS_BUILD)/services/%.o,$(patsubst services/grammar/%.c,$(DOS_BUILD)/services/%.o,$(SERVICES_SRC)))
# platform/common/ (T14.5): the action registry/keymap dispatch/status
# line/help pager shared by every backend -- mirrors ATARI_COMMON_OBJ.
DOS_COMMON_OBJ    = $(patsubst platform/common/%.c,$(DOS_BUILD)/common/%.o,$(PLATFORM_COMMON_SRC))

# platform/dos's own sources, excluding main.c (its own main() vs.
# dos_smoke.c's) -- mirrors ATARI_PLATFORM_SRC's identical filter-out.
# Defined here, BEFORE the SMOKE.EXE/RETROWP.EXE rules below that both
# reference it -- a real bug found directly, not a style choice: a
# rule's prerequisite list is expanded at the point the rule itself is
# PARSED, not lazily at build time, so a rule textually written before
# a recursively-expanded variable's definition silently sees it as
# empty forever, even though the variable gets a real value later in
# the same file. SMOKE.EXE's own link line was missing every platform/
# dos/*.o file this exact way on the first attempt (undefined mem_
# alloc_/plat_font_open_/etc. at link time) until this was moved up.
DOS_PLATFORM_SRC = $(filter-out platform/dos/main.c,$(wildcard platform/dos/*.c))
DOS_PLATFORM_OBJ = $(patsubst platform/dos/%.c,$(DOS_BUILD)/app/%.o,$(DOS_PLATFORM_SRC))

# dos-engine-check mirrors atari-engine-check/amiga-engine-check/
# mac-engine-check exactly: a link-free portability check of the SAME
# portable engine/ + offload/transport + services/ sources this project
# already tests on host, now proven under a 16-bit real-mode target too.
.PHONY: dos-engine-check
dos-engine-check: $(DOS_ENGINE_OBJ) $(DOS_TRANSPORT_OBJ) $(DOS_SERVICES_OBJ)
	@echo "engine + offload/transport + services cross-compile clean for Open Watcom DOS"

$(DOS_BUILD)/engine/%.o: engine/src/%.c | $(DOS_BUILD)/engine
	env $(DOS_ENV) $(DOSCC) $(DOS_STRICT_CFLAGS) $(DOS_INCLUDES) $(DOS_DEPFLAGS) -fo=$@ $<
	@$(call dos_depfix,$(@:.o=.d))

$(DOS_BUILD)/transport/%.o: offload/transport/%.c | $(DOS_BUILD)/transport
	env $(DOS_ENV) $(DOSCC) $(DOS_STRICT_CFLAGS) $(DOS_INCLUDES) $(DOS_DEPFLAGS) -fo=$@ $<
	@$(call dos_depfix,$(@:.o=.d))

$(DOS_BUILD)/services/%.o: services/spell/%.c | $(DOS_BUILD)/services
	env $(DOS_ENV) $(DOSCC) $(DOS_STRICT_CFLAGS) $(DOS_INCLUDES) $(DOS_DEPFLAGS) -fo=$@ $<
	@$(call dos_depfix,$(@:.o=.d))

$(DOS_BUILD)/services/%.o: services/grammar/%.c | $(DOS_BUILD)/services
	env $(DOS_ENV) $(DOSCC) $(DOS_STRICT_CFLAGS) $(DOS_INCLUDES) $(DOS_DEPFLAGS) -fo=$@ $<
	@$(call dos_depfix,$(@:.o=.d))

$(DOS_BUILD)/common/%.o: platform/common/%.c | $(DOS_BUILD)/common
	env $(DOS_ENV) $(DOSCC) $(DOS_STRICT_CFLAGS) $(DOS_INCLUDES) $(DOS_DEPFLAGS) -fo=$@ $<
	@$(call dos_depfix,$(@:.o=.d))

# Phase 0's own smoke test (M14 T14.6's go/no-go gate) was a
# freestanding program with no engine/platform-dos linkage -- platform/
# dos/ didn't exist yet. Phase 2 (T14.2) grew it into a real selfcheck
# runner (host/tools/dos_smoke.c's own wp_dos_layout_selfcheck call),
# so it now needs the full platform/dos + engine/transport/services
# object set, same as SMOKE.PRG/AMISMOKE each needed from very early on.
$(DOS_BUILD)/smoke/%.o: host/tools/%.c | $(DOS_BUILD)/smoke
	env $(DOS_ENV) $(DOSCC) $(DOS_RELAXED_CFLAGS) $(DOS_INCLUDES) $(DOS_DEPFLAGS) -fo=$@ $<
	@$(call dos_depfix,$(@:.o=.d))

$(DOS_BIN)/SMOKE.EXE: $(DOS_PLATFORM_OBJ) $(DOS_BUILD)/smoke/dos_smoke.o $(DOS_ENGINE_OBJ) $(DOS_COMMON_OBJ) $(DOS_TRANSPORT_OBJ) $(DOS_SERVICES_OBJ) | $(DOS_BIN)
	env $(DOS_ENV) $(DOSLINK) -0 -ml -bt=dos -k32768 $^ -fe=$@

.PHONY: dos-smoke
dos-smoke: $(DOS_BIN)/SMOKE.EXE

# T14.2 (Phase 2) is the first phase that needs the engine linked in --
# render_fixture_paragraph/wp_dos_layout_selfcheck call into docmodel/
# layout/format_txt -- and since every backend links its ENGINE_OBJ as
# one indivisible unit (not just the specific .c files a given feature
# happens to touch), engine/src/offload_client.c's own references to
# plat_transport_* must resolve too even though nothing calls
# offload_init() yet -- see platform/dos/transport_dos.c's own comment
# on why an honest "always fails to open" stub is correct here, not a
# placeholder.
# app_shell.c #includes the generated help_retrowp.h (see the Atari/
# Amiga rules above for the same dependency) -- missing here until
# M15 Phase 4 caught it via a clean `make dos`/`make dos-smoke` failing
# when build/generated/ didn't exist yet; a real, narrow, pre-existing
# Makefile gap, not new build logic.
$(DOS_BUILD)/app/app_shell.o: $(GENERATED)/help_retrowp.h

$(DOS_BUILD)/app/%.o: platform/dos/%.c | $(DOS_BUILD)/app
	env $(DOS_ENV) $(DOSCC) $(DOS_RELAXED_CFLAGS) $(DOS_INCLUDES) $(DOS_DEPFLAGS) -fo=$@ $<
	@$(call dos_depfix,$(@:.o=.d))

$(DOS_BIN)/RETROWP.EXE: $(DOS_PLATFORM_OBJ) $(DOS_BUILD)/app/main.o $(DOS_ENGINE_OBJ) $(DOS_COMMON_OBJ) $(DOS_TRANSPORT_OBJ) $(DOS_SERVICES_OBJ) | $(DOS_BIN)
	env $(DOS_ENV) $(DOSLINK) -0 -ml -bt=dos -k32768 $^ -fe=$@

$(DOS_BUILD)/engine $(DOS_BUILD)/transport $(DOS_BUILD)/services $(DOS_BUILD)/common $(DOS_BUILD)/smoke $(DOS_BUILD)/app $(DOS_BIN):
	mkdir -p $@

.PHONY: dos
dos: $(DOS_BIN)/RETROWP.EXE

clean:
	rm -rf $(BUILD) $(BIN)

# Every .d emitted above, if any. Deliberately last, and deliberately a
# wildcard rather than a derived list: on a clean tree there are no .d
# files and nothing is included, which is exactly right because a clean
# tree compiles everything regardless.
DEPFILES := $(shell find build -name '*.d' 2>/dev/null)
-include $(DEPFILES)
