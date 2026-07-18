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

clean:
	rm -rf $(BUILD) $(BIN)
