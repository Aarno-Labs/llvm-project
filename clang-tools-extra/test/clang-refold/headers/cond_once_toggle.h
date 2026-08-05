// A conditional that disables itself, so a second inclusion takes no arm and
// contributes no preprocessed tokens.
#if !defined(RF_TOGGLE_SEEN)
int cond_once_toggle_first = 1;
#define RF_TOGGLE_SEEN 1
#endif
