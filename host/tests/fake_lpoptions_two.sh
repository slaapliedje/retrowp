#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# A stand-in for lpoptions -l that answers for two printers, for the
# --disc-printer tests: "-p iP7200-series" is the Canon PIXMA iP7220's
# Gutenprint queue (trimmed), anything else the M477fdn's.
for a in "$@"; do
    if [ "$a" = "iP7200-series" ]; then
        cat <<'OPTS'
PageSize/Media Size: *Letter Legal Executive Postcard CD5Inch CD3Inch CDCustom A4
ColorModel/Color Model: Gray Black *RGB CMY CMYK KCMY
MediaType/Media Type: *Plain PhotoPlusGloss2 GlossyPaper DiscCompat DiscOthers Envelope
InputSlot/Media Source: *Cassette CD
Duplex/2-Sided Printing: *None DuplexNoTumble DuplexTumble
OPTS
        exit 0
    fi
done
exec host/tests/fake_lpoptions.sh
