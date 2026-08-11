// `__has_include` in an evaluated arm makes source materialization of this
// header unsound, so it is realized from the edited stream instead -- which
// drops the `#define` below and forces the carry path under test.
#if __has_include(<stddef.h>)
extern int selfref_obj;
int defs_v = 1;
// The self-referential idiom, as glibc spells stdin/stdout/stderr.
#define selfref_obj selfref_obj
#endif
