// A conditional that disables itself, so a second inclusion takes no arm.  The
// declaration ahead of it keeps the conditional from acting as an include
// guard, so the second inclusion is entered rather than skipped.
extern int cond_once_toggle_entries;
#if !defined(RF_TOGGLE_SEEN)
int cond_once_toggle_first = 1;
#define RF_TOGGLE_SEEN 1
#endif
