#include "defs.h"
// The payload names the macro as an ordinary token.  Expanding it is the
// identity, so carrying the definition in front of this payload is sound.
int payload_use = selfref_obj;
