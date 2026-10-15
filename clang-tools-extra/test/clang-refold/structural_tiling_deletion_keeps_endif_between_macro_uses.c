// RUN: %clang-refold-tester structural_tiling_deletion_keeps_endif_between_macro_uses
//
// Regression: a deletion spanning a preserved `#endif` is tiled around the
// directive even when the deleted text on each side of it spells a macro.
//
// B deletes both declarations, so A has one deletion hunk from `const` in the
// taken `#else` arm to the `;` after `sorts`.  The `#endif` must stay, so the
// tiler splits the hunk into two TU deletions and keeps the directive in place
// between them.
//
// Before this was fixed, the partition search also offered TU segments whose
// source swallowed the `#endif`, which no direct TU edit can realize.  Carving
// either `NIL` out as a macro segment then completed a mixed-realizer partition
// at the cost of the correct one, the tie was declined, and the hunk reached
// the terminal fallback as `NoOwnerClosedCover`.  With two `#endif` lines in
// the gap the spurious partitions were strictly cheaper; the `tree` program in
// the tenjin corpus failed that way deleting its `ftype` and `sorts` globals.
#define NIL ((void *)0)

#ifdef ALT
const char *names[] = {"alt", NIL};
#else
const char *names[] = {"file", NIL};
#endif

struct sorts {
  char *name;
} sorts[] = {{"none", NIL}};

int keep;
