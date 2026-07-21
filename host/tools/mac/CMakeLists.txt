# M12 CI/build fixtures -- Retro68/CMake-only, per docs/mac-platform.md's
# own explanation of why Mac resource-fork packaging can't be a plain
# Makefile recipe the way every other backend's build is. Build with:
#   mkdir -p build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=<retro68-build>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake -G "Unix Makefiles"
#   make
cmake_minimum_required(VERSION 3.10)
project(retrowp_mac C CXX)

set(RETROWP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# rwpci_launcher: injected exactly ONCE into the fixture template's own
# Startup Items (see host/tools/mac_hfs/), never rebuilt/reinjected per
# CI run -- only rebuild+reinject if launcher.c itself changes.
add_application(rwpci_launcher launcher.c)

# platform/mac's own sources, excluding the two files with their own
# main() (main.c for RETROWP, smoke.c for mac_smoke) -- everything else
# (app shell, mem/file/clipboard/timer/draw/metrics/dialog) is shared by
# both binaries, mirroring ATARI_PLATFORM_SRC/AMIGA_PLATFORM_SRC's own
# filter-out-main.c convention in the top-level Makefile.
file(GLOB RETROWP_MAC_PLATFORM_SRC ${RETROWP_ROOT}/platform/mac/*.c)
list(FILTER RETROWP_MAC_PLATFORM_SRC EXCLUDE REGEX ".*/main\\.c$")

# The whole portable engine, including offload_client.c now that
# transport_mac.c (Phase 4) implements plat_transport_* -- excluded up
# through Phase 3, same reasoning amiga_smoke.c's own Phase 1 Makefile
# rule gives for the identical exclusion, re-included once Amiga's own
# T9.3 landed transport_amiga.c. offload_client.c itself needs
# offload/transport's wire.c/caps_codec.c (framing/CRC and capability
# encoding), matching ATARI_TRANSPORT_OBJ/AMIGA_TRANSPORT_OBJ in the
# top-level Makefile.
file(GLOB RETROWP_ENGINE_SRC ${RETROWP_ROOT}/engine/src/*.c)
file(GLOB RETROWP_TRANSPORT_SRC ${RETROWP_ROOT}/offload/transport/*.c)

set(RETROWP_COMMON_SRC
    ${RETROWP_ROOT}/platform/common/actions.c
    ${RETROWP_ROOT}/platform/common/app_ctx.c
    ${RETROWP_ROOT}/platform/common/keymap.c
)

set(RETROWP_INCLUDES
    ${RETROWP_ROOT}/engine/include
    ${RETROWP_ROOT}/platform/mac
    ${RETROWP_ROOT}/platform/common
    ${RETROWP_ROOT}/offload/transport
)

# mac_smoke: rebuilt every CI run, copied fresh onto the ExtFS share as
# CI_TARGET.APPL by host/tools/mac_ci_lib.sh.
add_application(mac_smoke
    smoke.c
    ${RETROWP_MAC_PLATFORM_SRC}
    ${RETROWP_COMMON_SRC}
    ${RETROWP_ENGINE_SRC}
    ${RETROWP_TRANSPORT_SRC}
)
target_include_directories(mac_smoke PRIVATE ${RETROWP_INCLUDES})

# RETROWP: the real application shell (Phase 2 onward).
add_application(RETROWP
    ${RETROWP_ROOT}/platform/mac/main.c
    ${RETROWP_MAC_PLATFORM_SRC}
    ${RETROWP_COMMON_SRC}
    ${RETROWP_ENGINE_SRC}
    ${RETROWP_TRANSPORT_SRC}
)
target_include_directories(RETROWP PRIVATE ${RETROWP_INCLUDES})
