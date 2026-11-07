#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# A stand-in for CUPS's lpoptions -l, for the OFFLOAD_PRINT_OPTIONS tests:
# the HP Color LaserJet Pro MFP M477fdn queue's own answer, trimmed, with
# the duplex PPD (M477d) and its duplex unit installed.
cat <<'OPTS'
PageSize/Media Size: *Letter Legal Executive A4 A5
Collate/Collate: True *False
InputSlot/Paper Feed: *Auto Tray1 Tray2 Tray3 ManualFeed
Duplex/Two-Sided: *None DuplexNoTumble DuplexTumble
HPOption_Duplexer/Duplex Unit: *True False
HPPJLEconoMode2/EconoMode: on *off
HPPJLColorAsGray/Print Color as Gray: on *off
OPTS
