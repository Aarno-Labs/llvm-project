// The operator is hidden behind a macro, so a textual scan of the arm
// condition ("HAS_COMPANION(...)") cannot see `__has_include`. Only the
// producer-recorded evaluation fact exposes it.
#define HAS_COMPANION(x) __has_include(x)
#if HAS_COMPANION("companion.h")
int values[] = { 1, 3 };
#else
int values[] = { 9 };
#endif
