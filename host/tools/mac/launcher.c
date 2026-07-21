/* Permanent Startup Items launcher stub (M12 Phase 0).

   BasiliskII has no equivalent to amiberry's host-directory-mountable
   boot volume, so there is no way to put a Startup-Sequence-style CI
   hook directly on the boot volume's own filesystem the way
   amiga_ci_lib.sh does. This stub is the workaround: injected exactly
   ONCE into System Folder:Startup Items on the fixture's boot volume
   (see host/tools/mac_hfs/inject_startup_item.py and
   docs/mac-platform.md), it auto-launches at every boot with zero
   Finder interaction, and its only job is to hand off execution to
   whatever the CI harness has most recently copied onto the ExtFS
   "Unix:" share as CI_TARGET.APPL -- a plain host-file copy, no disk-
   image surgery needed per run. This file itself never needs to
   change (and the injected copy in the fixture template never needs
   to be re-injected) unless this launch logic itself changes.

   Retro68's per-topic headers (Processes.h, Files.h, ...) are thin
   wrappers around one master header with the real declarations --
   LaunchApplication really is available here, trap 0xA9F2, despite
   Processes.h itself looking like an empty stub at a glance. */
#include <Types.h>
#include <Files.h>
#include <Processes.h>
#include <Quickdraw.h>
#include <stdio.h>

static void write_marker(const char *path, const char *msg)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(msg, 1, 4, f);
        fclose(f);
    }
}

int main(void)
{
    FSSpec spec;
    LaunchParamBlockRec lpb;
    OSErr err;

    err = FSMakeFSSpec(0, 0, "\pUnix:CI_TARGET.APPL", &spec);
    if (err != noErr) {
        write_marker("Unix:launcher_fsspec_fail.txt", "FAIL");
        ExitToShell();
        return 0;
    }

    lpb.launchBlockID = extendedBlock;
    lpb.launchEPBLength = extendedBlockLen;
    lpb.launchFileFlags = 0;
    lpb.launchControlFlags = launchContinue;
    lpb.launchAppSpec = &spec;
    lpb.launchAppParameters = NULL;

    err = LaunchApplication(&lpb);
    if (err != noErr) {
        write_marker("Unix:launcher_launch_fail.txt", "FAIL");
    }

    ExitToShell();
    return 0;
}
