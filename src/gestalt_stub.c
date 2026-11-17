/* Gestalt was removed from the system library. Praat still calls it for
   the CoreAudio host and the sound recorder. */
#include <CoreServices/CoreServices.h>

OSErr Gestalt(OSType selector, SInt32 *response) {
	if (!response)
		return -1;
	if (selector == gestaltSystemVersionMajor) {
		*response = 15;
		return noErr;
	}
	if (selector == gestaltSystemVersionMinor || selector == gestaltSystemVersionBugFix) {
		*response = 0;
		return noErr;
	}
	if (selector == gestaltSystemVersion) {
		*response = 0x10F0;
		return noErr;
	}
	/* Sound-attribute bits: report every legacy input feature as present. */
	*response = ~0;
	return noErr;
}
