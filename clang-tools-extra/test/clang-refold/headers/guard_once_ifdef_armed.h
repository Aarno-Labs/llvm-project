// `#pragma once` that fires only once the includer has defined
// GUARD_ONCE_ARMED, so an earlier inclusion re-enters and a later one does not.
#ifdef GUARD_ONCE_ARMED
#pragma once
#endif
int guard_once_armed_body = 1;
