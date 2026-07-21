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
# CI_TARGET.APPL by host/tools/mac_ci_lib.sh.
add_application(mac_smoke smoke.c)
