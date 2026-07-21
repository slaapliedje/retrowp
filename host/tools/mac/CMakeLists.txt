# M12 Phase 0 CI fixtures -- Retro68/CMake-only, per docs/mac-platform.md's
# own explanation of why Mac resource-fork packaging can't be a plain
# Makefile recipe the way every other backend's build is. Build with:
#   mkdir -p build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=<retro68-build>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake -G "Unix Makefiles"
#   make
cmake_minimum_required(VERSION 3.10)
project(retrowp_mac_ci C CXX)

# rwpci_launcher: injected exactly ONCE into the fixture template's own
# Startup Items (see host/tools/mac_hfs/), never rebuilt/reinjected per
# CI run -- only rebuild+reinject if launcher.c itself changes.
add_application(rwpci_launcher launcher.c)

# mac_smoke: rebuilt every CI run, copied fresh onto the ExtFS share as
# CI_TARGET.APPL by host/tools/mac_ci_lib.sh. Links platform/mac's own
# non-UI seams (Phase 1) plus engine/src/utf8.c specifically -- utf8.c is
# a leaf module (only includes utf8.h) that draw_mac.c's
# plat_utf8_to_native/plat_native_to_utf8 call directly (platform/ may
# call engine/ utility functions; the reverse is what's forbidden).
# Deliberately NOT the full engine object set: that would pull in
# linebreak.o/offload_client.o's plat_font_*/plat_transport_* references
# before Phase 2/4 exist to satisfy them -- same reasoning
# amiga_smoke.c's own Phase 1 Makefile rule gives.
set(RETROWP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_application(mac_smoke
    smoke.c
    ${RETROWP_ROOT}/platform/mac/app_shell.c
    ${RETROWP_ROOT}/platform/mac/mem_mac.c
    ${RETROWP_ROOT}/platform/mac/file_mac.c
    ${RETROWP_ROOT}/platform/mac/clipboard_mac.c
    ${RETROWP_ROOT}/platform/mac/timer_mac.c
    ${RETROWP_ROOT}/platform/mac/draw_mac.c
    ${RETROWP_ROOT}/engine/src/utf8.c
)
target_include_directories(mac_smoke PRIVATE
    ${RETROWP_ROOT}/engine/include
    ${RETROWP_ROOT}/platform/mac
)
