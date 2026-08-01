// The `__has_include` here sits in a #elif that is never reached: the #if 1
// arm above it is unconditionally selected, so the preprocessor never
// evaluates the operator. A textual scan of the arm condition would wrongly
// flag it; the producer records no evaluation, so the header stays soundly
// source-replayable after materialization.
#if 1
int values[] = { 1, 3 };
#elif __has_include("companion.h")
int values[] = { 9 };
#endif
