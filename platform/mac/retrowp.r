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
