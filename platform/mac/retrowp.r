/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RetroWP's 'SIZE': Retro68's default (Retro68APPL.r), but high-level-
   event aware -- without it the Finder's Apple Events (open these
   documents, quit) never reach the application. Same memory sizes. */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved,
	ignoreSuspendResumeEvents,
	reserved,
	cannotBackground,
	needsActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	isHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	1024 * 1024,
	1024 * 1024
};

/* The Finder's bundle: RetroWP's signature 'RWP1' -- the creator its
   saved documents carry (retroplat's file_mac.c) -- and its icons, the
   application's and its documents' (icons.r, generated). The 'TEXT'
   entry also lets any text file be dropped on the application, which
   opens it by Apple Event like a double-click. */
#include "icons.r"

type 'RWP1' as 'STR ';
resource 'RWP1' (0, "Owner resource") {
	"RetroWP"
};

resource 'FREF' (128) { 'APPL', 0, "" };
resource 'FREF' (129) { 'TEXT', 1, "" };

resource 'BNDL' (128) {
	'RWP1',
	0,
	{
		'ICN#', { 0, 128; 1, 129 };
		'FREF', { 0, 128; 1, 129 }
	}
};
