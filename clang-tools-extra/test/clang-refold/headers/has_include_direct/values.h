// Direct (non-macro-hidden) use of the operator. The producer records the
// evaluation just as it does for the macro-hidden form, so a materialized
// edit here is realized from B rather than source-replayed.
#if __has_include("companion.h")
int values[] = { 1, 3 };
#else
int values[] = { 9 };
#endif
